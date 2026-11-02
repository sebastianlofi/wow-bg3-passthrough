-- Inventory (server; party-multiplayer-design.md "Inventory"): one
-- inventory, seen in BG3's screens. Every WoW item a party member's WoW character holds is a real
-- BG3 item built on the nearest BG3 base item, with the WoW name, icon and mapped stats; WoW is the
-- truth and BG3's changes are requests to it. Full description: the inventory notes.
--
-- Data: wow-bg3/link/items.json (link/tools/wow_items.py: WoW entry -> BG3 base stats entry, name,
-- icon, mapped numbers). Each entry in use gets its own stats entry "WOWBG3_I_<entry>"
-- (Ext.Stats.Create with the base as parent; made again at StatsLoaded from inventory-used.json so a
-- save's items find them) and translated strings "WowBg3_N<entry>" / "WowBg3_D<entry>" (name, WoW's
-- own tooltip text). Stackable things are one BG3 stack per member and entry ("S<entry>@<member>").
-- A BG3 item for a WoW item: the base's root template made with our stats (the root template's Stats
-- swapped for the one CreateAt, so BG3 builds the weapon/armour components from them), then its own
-- cache template (esv::Item::CreateCacheTemplate, what BG3 does for a changed item; saved with it)
-- carrying our stats, name, description and icon ("WowBg3_<wow icon>" from the atlas that
-- link/tools/wow_icons.py builds and InventoryClient.lua puts in place). The item remembers its WoW
-- item in the persistent user variable WowBg3Item ("<wow item guid>"; "L<corpse>:<idx>" for corpse
-- loot, "V<npc>:<idx>" for vendor wares).
--
-- Sync: link/tools/inventory_bridge.py writes wow-bg3/link/inventory.json (every member's WoW items
-- and money, vendor wares, corpse loot, replies); Reconcile() makes BG3 match it (create, equip or
-- unequip, stack counts, gold, remove what WoW no longer has). BG3's own moves (Equipped, Unequipped,
-- AddedTo between members, vendor and corpse) become requests in wow-bg3/link/inventory-req.json;
-- what the server refuses is undone by the next reconcile. Echoes of our own changes are ignored.
-- Status: wow-bg3/link/inventory-status.json. Research (harness): wow-bg3/inventory-research.lua.
local M = {}
local Harness = Ext.Require("Harness.lua")

local IO = "wow-bg3/link/"
local VAR = "WowBg3Item"
local PREFIX_STATS = "WOWBG3_I_"
local client = Ext.IsClient()
local tick = 0 -- server ticks since Start (all timing below counts these)
-- Hero.lua (the controlled character); a stand-in when loaded outside bootstrap (harness research).
local Camp = Ext.Require("Camp.lua") -- the switch `camp`
local Prices = Ext.Require("Prices.lua")
local Stock = Ext.Require("Stock.lua")
local okHero, Hero = pcall(Ext.Require, "Hero.lua")
if not okHero or type(Hero) ~= "table" then
    Hero = {Uuid = function()
        local h = Osi.GetHostCharacter()
        return type(h) == "string" and h:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil
    end}
end
pcall(Ext.Vars.RegisterUserVariable, VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})
-- On an item: its WoW entry (the stats entry name says it too, but only once that entry exists).
local ENTRY_VAR = "WowBg3Entry"
pcall(Ext.Vars.RegisterUserVariable, ENTRY_VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})
-- On an item: the BG3 base it was made from. Its template stays that base's, whatever the table says later.
local BASE_VAR = "WowBg3Base"
pcall(Ext.Vars.RegisterUserVariable, BASE_VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})
-- On a party member: the boosts of the WoW-only pieces (shoulders, legs, ...) its WoW character wears.
local WORN_VAR = "WowBg3Worn"
pcall(Ext.Vars.RegisterUserVariable, WORN_VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})
local wornLooked = {} -- member -> this Lua state (a load rebuilds it) has read its boosts
local function hasWornBoost(e)
    local ok, found = pcall(function()
        for _, b in ipairs(e.BoostsContainer.Boosts) do
            for _, x in ipairs(b.Boosts) do
                if tostring(x.BoostInfo.Cause.Cause) == "WowBg3Worn" then return true end
            end
        end
        return false
    end)
    return ok and found
end

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
-- Every BG3 item of ours (Ext.Vars.GetEntitiesWithVariable gives UUID strings). A second BG3 item
-- for the same WoW item (never wanted) is deleted here.
local function eachOurs(fn)
    for _, x in pairs(q(Ext.Vars.GetEntitiesWithVariable, VAR) or {}) do
        local u = type(x) == "string" and x or q(function() return x.Uuid.EntityUuid end)
        local e = u and q(Ext.Entity.Get, u)
        local v = e and q(function() return e.Vars[VAR] end)
        if type(v) == "string" then fn(u, v, e) end
    end
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end
M.UuidOf = uuidOf

-- ---- the item table ----
local items, iconSet = nil, nil
function M.Items()
    if items then return items end
    items = {}
    local raw = Ext.IO.LoadFile(IO .. "items.json")
    local ok, t = pcall(Ext.Json.Parse, raw or "")
    if ok and type(t) == "table" and type(t.Items) == "table" then
        for _, r in ipairs(t.Items) do items[r.e] = r end
        M.CopperPerGold = t.CopperPerGold or 1
        -- The effects switch (wow_items.py --effects): absent from the table when off.
        M.EffectRules = t.Effects
        for _, p in ipairs(t.Props or {}) do M.Props = M.Props or {}; M.Props[p.id] = p end
        M.Pools = t.Pools
    end
    return items
end
function M.Item(entry) return M.Items()[math.tointeger(entry) or -1] end

-- A WoW random suffix ("of the Bear"): the same row and stats entry with the suffix in its name and
-- text under its own string handles; what the suffix gives comes through M.EffectBoosts.
function M.Variant(row, rand)
    M.Items()
    local id = rand and math.tointeger(rand)
    local p = id and M.Props and M.Props[id]
    if not p then return row end
    return setmetatable({
        NameKey = M.NameHandle(row.e) .. "_" .. tostring(id),
        DescKey = M.DescHandle(row.e) .. "_" .. tostring(id),
        n = (row.n or "") .. " " .. p.n,
        wd = (row.wd and row.wd .. "\n" or "") .. (p.t or ""),
    }, {__index = row})
end

-- What a member wears (WoW's list), on-equip points and suffix points added up by kind, as boosts.
function M.EffectBoosts(wornItems)
    M.Items()
    local rules, out = M.EffectRules, {}
    if not rules then return out end
    local pts = {}
    local function add(ef) for kind, v in pairs(ef or {}) do pts[kind] = (pts[kind] or 0) + v end end
    for _, it in ipairs(wornItems) do
        if it.Bag == 255 and it.Slot < 19 then
            add((M.Item(it.Entry) or {}).ef)
            add(((M.Props or {})[it.Rand or 0] or {}).ef)
        end
    end
    -- One boost for two kinds (attack power, ranged attack power): the larger total counts, not their sum.
    for kind, r in pairs(rules) do
        if r.into and pts[kind] then
            pts[r.into] = math.max(pts[r.into] or 0, pts[kind])
            pts[kind] = nil
        end
    end
    -- One boost for two kinds (attack power, ranged attack power): the larger total counts, not their sum.
    for kind, r in pairs(rules) do
        if r.into and pts[kind] then
            pts[r.into] = math.max(pts[r.into] or 0, pts[kind])
            pts[kind] = nil
        end
    end
    for kind, v in pairs(pts) do
        local r = rules[kind]
        if r then
            local n = math.floor(math.abs(v) / r.per + 0.000001)
            n = math.floor(math.min(n, r.cap)) * (v < 0 and -1 or 1)
            if n > 0 or (n < 0 and r.b:find("{[np]}")) then -- a boost with no number has no minus side
                local text = r.b:gsub("{n}", tostring(n)):gsub("{p}", tostring(100 + n))
                for b in text:gmatch("[^;]+") do out[#out + 1] = b end
            end
        end
    end
    return out
end

-- Icons the atlas holds (wow_icons.py): only those names are given to items, others keep the base's.
function M.IconName(row)
    if not iconSet then
        iconSet = {}
        local raw = Ext.IO.LoadFile("wow-bg3/gui/wow-icons.json")
        local ok, t = pcall(Ext.Json.Parse, raw or "")
        if ok and type(t) == "table" then for _, n in ipairs(t.Icons or {}) do iconSet[n] = true end end
    end
    if not row or type(row.i) ~= "string" or row.i == "" then return nil end
    local key = "WowBg3_" .. row.i:lower():gsub("[^a-z0-9_]", "_")
    return iconSet[key] and key or nil
end

function M.StatsName(entry) return PREFIX_STATS .. tostring(entry) end
function M.NameHandle(entry) return "WowBg3_N" .. tostring(entry) end
function M.DescHandle(entry) return "WowBg3_D" .. tostring(entry) end

-- ---- stats and strings for the entries in use (both sides) ----
local WEAPON_SIDE = {WeaponEnchantment = true, WeaponDamage = true, WeaponProperty = true}
local function statsType(row)
    if row.k == "weapon" then return "Weapon" end
    if row.k == "armor" then return "Armor" end
    return "Object"
end
-- Entries ever used (wow-bg3/link/inventory-used.json): their stats entries are made at StatsLoaded on
-- both sides, before a save's items load (Ext.Stats.SetPersistence kept nothing for the unpacked
-- harness mod: after a load the items had no stats, harness).
local USED = IO .. "inventory-used.json"
M.Used, M.UsedDirty = {}, false
local function loadUsed()
    local ok, t = pcall(Ext.Json.Parse, Ext.IO.LoadFile(USED) or "")
    if ok and type(t) == "table" then for _, e in ipairs(t.Entries or {}) do M.Used[e] = true end end
end
function M.SaveUsed()
    if not M.UsedDirty then return end
    M.UsedDirty = false
    local list = {}
    for e in pairs(M.Used) do list[#list + 1] = e end
    table.sort(list)
    Ext.IO.SaveFile(USED, Ext.Json.Stringify({Schema = 1, Entries = list}))
end

-- ---- prices (the rule: Prices.lua) ----
-- BG3's trade prices read the item's own value (e.Value.Value), not its stats entry, so a merchant's
-- ware and the party's copy of the same entry each hold their own: the screen shows WoW's buy price for
-- the one and WoW's sell price for the other, with attitude and Persuasion on top. BG3 keeps the value
-- in the save, so an item from an older save gets its value here too.
local TRADE_MODIFIER = "TRADE_PRICE_MODIFIER_f52ca62e-e7ee-435a-853f-454b2303c278"
local tradeBase = Prices.DEFAULT_BASE
-- The save's own (it goes with the difficulty, and the player can change that in a running game).
function M.TradeBase()
    local v = q(Osi.GetRulesetModifierFloat, TRADE_MODIFIER)
    if type(v) == "number" and v >= 1 then tradeBase = v end
    return tradeBase
end
-- A BG3 thrown dagger is one item, as BG3's own are, and stands for a whole WoW stack: a ware for WoW's lot (lp), the
-- party's for what its WoW stack holds (count), which is what WoW buys and sells when it moves.
function M.ValueOf(row, ware, count)
    local cpg = M.CopperPerGold or 1
    local bp = (row.bp or 0) / cpg
    if ware then return Prices.Ware((row.lp or row.bp or 0) / cpg, tradeBase) end
    return Prices.Held((row.sp or 0) * (count or 1) / cpg, bp, tradeBase)
end
-- How many BG3 holds for WoW's count: always one dagger.
function M.AmountOf(row, count) return row.s == "Thrown" and 1 or count end
local function setValue(e, want)
    local ok, cur = pcall(function() return e.Value.Value end)
    if not ok or cur == want then return false end
    if not pcall(function() e.Value.Value = want end) then return false end
    pcall(function() e:Replicate("Value") end) -- the client's trade screen works it out from its own copy
    return true
end

local makeStats -- below
function M.MakeStats(row)
    local name = M.StatsName(row.e)
    if not M.Used[row.e] then M.Used[row.e] = true; M.UsedDirty = true end
    if Ext.Stats.Get(name) then return name end
    assert(not client, "stats are made on the server")
    return makeStats(row, true)
end
makeStats = function(row, sync)
    local name = M.StatsName(row.e)
    local base = Ext.Stats.Get(row.b)
    if not base then return nil end
    local s = Ext.Stats.Create(name, statsType(row), row.b)
    if not s then return nil end
    local default, boosts = {}, {}
    for b in tostring(row.bo or ""):gmatch("[^;]+") do
        local head = b:match("^(%a+)")
        if row.k == "weapon" and WEAPON_SIDE[head] then default[#default + 1] = b else boosts[#boosts + 1] = b end
    end
    if row.k == "weapon" and row.en then default[#default + 1] = "WeaponProperty(Magical)" end
    pcall(function()
        if row.k == "weapon" then
            if row.dt then s["Damage Type"] = row.dt end
            if #default > 0 then
                local old = tostring(base.DefaultBoosts or "")
                s.DefaultBoosts = (old ~= "" and old .. ";" or "") .. table.concat(default, ";")
            end
        end
        if row.k == "armor" and row.ac then s.ArmorClass = row.ac end
        if #boosts > 0 then
            local old = tostring(base.Boosts or "")
            s.Boosts = (old ~= "" and old .. ";" or "") .. table.concat(boosts, ";")
        end
        if row.r then s.Rarity = row.r end
        -- Only where a new item's value starts: M.ValueOf sets each item's own from then on.
        s.ValueOverride = M.ValueOf(row, false)
    end)
    -- WoW's food and drink are BG3's camp supplies (switch `camp`; `sv` from wow_items.py). BG3's water has
    -- no supply value and no supply tag of its own, its food has both.
    if row.sv and Camp.On() then
        pcall(function()
            s.SupplyValue = row.sv
            local old = tostring(base.DefaultBoosts or "")
            if not old:find("CAMPSUPPLIES", 1, true) then
                s.DefaultBoosts = (old ~= "" and old .. ";" or "") .. "Tag(CAMPSUPPLIES)"
            end
        end)
    end
    if sync then pcall(function() s:Sync() end) end
    return name
end

function M.MakeStrings(row)
    Ext.Loca.UpdateTranslatedString(row.NameKey or M.NameHandle(row.e), row.n or ("WoW item " .. row.e))
    local desc = row.wd or ""
    if row.k == "worn" then desc = "Worn in WoW (" .. tostring(row.s) .. ").\n" .. desc end
    Ext.Loca.UpdateTranslatedString(row.DescKey or M.DescHandle(row.e), desc)
end

-- Strings for the entries in use only, like their stats entries (M.MakeStats: Ext.Stats.Create costs grow with
-- the number of entries; all 17,707 at StatsLoaded took over 160 s in the harness). BG3's string table has room
-- for about 28,000 new handles until the engine grows it itself, and bg3le never reallocates it: all 170,772
-- (every entry and suffix, name and text) left 142,403 refused ("loca: no room to add" in bg3le.log), every suffix
-- and 3,522 base names among them, and a later write of the same handle was refused as well. Client and server
-- share the table, so an item's strings go in when an item is made or learns its suffix (M.Create, M.Redress, the
-- reconcile), and the used entries' at each load for the items in the save.
local made = false
-- At StatsLoaded (both sides): the stats entries of every entry used before.
function M.MakeUsed()
    loadUsed()
    local t0, n = Ext.Utils.MonotonicTime(), 0
    for e in pairs(M.Used) do
        local row = M.Item(e)
        if row and not Ext.Stats.Get(M.StatsName(e)) and pcall(makeStats, row, false) then n = n + 1 end
    end
    M.UsedMs = Ext.Utils.MonotonicTime() - t0
    Ext.Log.Print(string.format("WOW-BG3 inventory: %d used item stats in %d ms (%s)", n, M.UsedMs, client and "client" or "server"))
end

function M.MakeAll()
    if made then return end
    made = true
    loadUsed()
    local t0, n = Ext.Utils.MonotonicTime(), 0
    for e in pairs(M.Used) do
        local row = M.Item(e)
        if row and pcall(M.MakeStrings, row) then n = n + 1 end
    end
    M.MadeMs = Ext.Utils.MonotonicTime() - t0
    Ext.Log.Print(string.format("WOW-BG3 inventory: %d item names in %d ms (%s)", n, M.MadeMs, client and "client" or "server"))
end

-- ---- making a BG3 item for a WoW item ----
local function rootOf(row)
    local base = Ext.Stats.Get(row.b)
    local rt = base and base.RootTemplate
    if type(rt) ~= "string" or rt == "" then return nil end
    return rt
end

-- Name and icon on a BG3 item. BG3 keeps them in components built from the template when the item
-- is made (DisplayName, the server's ServerDisplayNameList, whose first entry is the template's name,
-- Icon and ServerIconList), so the cache template alone changes nothing on a live item (harness
-- tests). Both are set and replicated; after a load Redress() sets them again from the item's
-- stats entry (WOWBG3_I_<entry>).
local function setHandle(ts, handle)
    ts.Handle.Handle = handle
    ts.Handle.Version = 0
end
function M.Dress(u, row)
    local e = Ext.Entity.Get(u)
    if not e then return false end
    local name = row.NameKey or M.NameHandle(row.e)
    pcall(setHandle, e.DisplayName.Name, name)
    pcall(function() setHandle(e.ServerDisplayNameList.Names[1].NameKey, name) end)
    local icon = M.IconName(row)
    if icon then
        pcall(function() e.Icon.Icon = icon end)
        pcall(function() e.ServerIconList.Icons[1].Icon = icon end)
    end
    pcall(function() e:Replicate("DisplayName") end)
    if icon then pcall(function() e:Replicate("Icon") end) end
    return true
end

-- The item's own (cache) template: our stats, name, description and icon (saved with the item).
local function dress(e, row)
    local t = e.ServerItem:CreateCacheTemplate()
    pcall(function() t.Stats = M.StatsName(row.e) end)
    pcall(function() t.DisplayName = row.NameKey or M.NameHandle(row.e) end) -- a TranslatedString is its handle in Lua
    pcall(function() t.Description = row.DescKey or M.DescHandle(row.e) end)
    local icon = M.IconName(row)
    if icon then pcall(function() t.Icon = icon end) end
    return t
end

-- Every item of ours again (after a load: names and icons come back from the template otherwise).
-- An item whose stats entry was missing when the save loaded (its entry not yet in inventory-used.json)
-- comes back without stats: no Data.StatsId, no Equipable, so worn armour draws nothing and BG3 hides
-- the body part under it (a playtest report: no feet in the world or the examine screen; harness: his
-- WoW boots, entry 140, were in neither his nor the harness's used list). Such an item is removed (its
-- entry now marked used) and the next reconcile makes it again from WoW's list, equipped if WoW wears it.
-- A thrown weapon from a save made before it was a dagger goes the same way: it has the dart's template and
-- may sit in BG3's ranged slot, which BG3 does not throw from.
M.Remade = 0
function M.OldThrown(row, base) return row.s == "Thrown" and base ~= row.b end
local suffixed = {} -- uuid -> the suffix its name shows (reconcile)
function M.Redress()
    local n, broken = 0, {}
    suffixed = {}
    eachOurs(function(u, _, e)
        local entry = q(function() return e.Vars[ENTRY_VAR] end)
        local sid = q(function() return e.Data.StatsId end)
        if sid == "" then broken[#broken + 1] = {u, entry} return end
        if math.type(entry) ~= "integer" then
            entry = type(sid) == "string" and tonumber(sid:match("^" .. PREFIX_STATS .. "(%d+)$"))
        end
        local row = entry and M.Item(entry)
        if row then pcall(M.MakeStrings, row) end
        if row and M.OldThrown(row, q(function() return e.Vars[BASE_VAR] end)) then
            broken[#broken + 1] = {u, entry}
        elseif row and M.Dress(u, row) then n = n + 1 end
    end)
    for _, b in ipairs(broken) do
        if math.type(b[2]) == "integer" and not M.Used[b[2]] then M.Used[b[2]] = true; M.UsedDirty = true end
        M.Suppress(b[1])
        if pcall(Osi.RequestDelete, b[1]) then M.Remade = M.Remade + 1 end
    end
    if #broken > 0 then pcall(M.SaveUsed) end
    return n
end

M.Fresh = {}
-- A new item is dressed again 10 ticks on. Why was never shown: "components settle" dates from the first version,
-- when the blank names this may have been written for had another cause (M.MakeAll's note). So it says what it
-- finds: if every line in a session of buying and looting says "as dressed", this goes, M.Fresh with it.
function M.DressFresh(u, row)
    local e = Ext.Entity.Get(u)
    local name = row.NameKey or M.NameHandle(row.e)
    local function read(f) local ok, v = pcall(f); return ok and tostring(v) or "none" end
    local shown = read(function() return e.DisplayName.Name.Handle.Handle end)
    local listed = read(function() return e.ServerDisplayNameList.Names[1].NameKey.Handle.Handle end)
    local icon = read(function() return e.Icon.Icon end)
    local want = M.IconName(row)
    local same = shown == name and listed == name and (not want or icon == want)
    Ext.Log.Print(string.format("WOW-BG3 inventory: fresh item %s: %s (name %s, list %s, icon %s, want %s, %s)", tostring(u),
        same and "as dressed" or "had changed", shown, listed, icon, name, tostring(want)))
    return M.Dress(u, row)
end
-- A new BG3 item for WoW entry `entry` (count for stacks) in `holder`'s inventory; tag = WowBg3Item.
function M.Create(entry, holder, count, tag, rand)
    local row = M.Variant(assert(M.Item(entry), "no item " .. tostring(entry)), rand)
    count = M.AmountOf(row, count)
    M.MakeStats(row)
    M.MakeStrings(row)
    local rt = assert(rootOf(row), "no root template for " .. tostring(row.b))
    local root = Ext.Template.GetRootTemplate(rt)
    local x, y, z = Osi.GetPosition(holder)
    local old = root.Stats
    root.Stats = M.StatsName(row.e)
    local ok, item = pcall(Osi.CreateAt, rt, x, y, z, 0, 0, "")
    root.Stats = old
    assert(ok and item, "CreateAt: " .. tostring(item))
    local u = uuidOf(item)
    local e = Ext.Entity.Get(u)
    dress(e, row)
    M.Dress(u, row)
    setValue(e, M.ValueOf(row, tostring(tag):sub(1, 1) == "V"))
    M.Fresh[u] = {Row = row, Tick = tick} -- dressed again a few ticks later (see M.DressFresh)
    e.Vars[VAR] = tag
    e.Vars[ENTRY_VAR] = row.e
    e.Vars[BASE_VAR] = row.b
    M.Suppress(u)
    -- The whole stack before it moves: BG3 sends camp supplies on to the Camp Supply Sack, and of a stack
    -- raised after the move only the moved 1 went, a stray bare copy WoW's list does not know.
    if count and count > 1 then pcall(Osi.SetStackAmount, u, count) end
    Osi.ToInventory(u, holder, count or 1, 0, 0)
    return u
end

-- ---- echo suppression: our own Equip/ToInventory calls fire the same events as the player's ----
local suppress = {}
function M.Suppress(u) suppress[u] = tick + 20 end
local function suppressed(u) return suppress[u] ~= nil and suppress[u] >= tick end

-- ---- requests to WoW ----
local reqs, reqSeq, runId = {}, 0, nil
local pending = {} -- Seq -> {Op, Item, Member, ...}
-- BG3 items with a request on the way: the reconcile leaves them alone until the reply (or 30 s).
local pendingItem = {} -- uuid -> tick asked
-- A barter that went through: member -> {At, Target = its BG3 gold after it} until WoW's money is that (reconcile).
M.Barter = {}
local BARTER_OPS = {buy = true, buypart = true, sell = true}
local BARTER_WAIT = 600
function M.Request(r)
    reqSeq = reqSeq + 1
    r.Seq = reqSeq
    if r.Item then pendingItem[r.Item] = tick end
    if M.NotePrice then M.NotePrice(r) end
    reqs[#reqs + 1] = r
    while #reqs > 64 do table.remove(reqs, 1) end
    pending[reqSeq] = r
    Ext.IO.SaveFile(IO .. "inventory-req.json", Ext.Json.Stringify({RunId = runId, Reqs = reqs}))
    return reqSeq
end

-- ---- what BG3 holds ----
local function tagOf(u)
    local e = q(Ext.Entity.Get, u)
    local v = e and q(function() return e.Vars[VAR] end)
    return type(v) == "string" and v or nil
end
M.TagOf = tagOf

-- (eachOurs: near the top)
M.Dupes = 0
local function ours()
    local out = {} -- tag -> uuid
    eachOurs(function(u, v)
        if out[v] and out[v] ~= u then
            M.Dupes = M.Dupes + 1
            M.Suppress(u)
            pcall(Osi.RequestDelete, u)
        else
            out[v] = u
        end
    end)
    return out
end

local function holderOf(u) return uuidOf(q(Osi.GetInventoryOwner, u)) end
-- A plain WoW item's tag: its guid in hex as the bridge lists it ("6F"). Stacks, wares and loot start S, V, L.
local function isWowGuid(tag) return tag:match("^%x+$") ~= nil end
local function equipped(u) return q(Osi.IsEquipped, u) == 1 end

-- ---- reconcile BG3 with inventory.json ----
local state = {Seq = -1, Members = {}, Created = 0, Errors = {}}
local lastInv = {} -- the last inventory.json
M.State = state
local wowOwner = {} -- wow guid -> member uuid
local corpseOf, vendorOf = {}, {} -- stand-in uuid -> wow guid hex
M.CorpseOf, M.VendorOf = corpseOf, vendorOf

local function note(err)
    state.Errors[#state.Errors + 1] = tostring(err)
    while #state.Errors > 20 do table.remove(state.Errors, 1) end
end

local function goldOf(u) return q(Osi.GetGold, u) or 0 end
-- A barter's request: what WoW says it is worth, and WoW's money before the deal, for a refusal in it (the Gold part below).
function M.NotePrice(r)
    local b = M.Barter[r.Member]
    if not (BARTER_OPS[r.Op] and b) then return end
    local m = (lastInv.Members or {})[r.Member] or {}
    if b.Target then -- a new deal right after a refusal was settled: it starts from there
        b = {At = tick, Money = b.Target}
        M.Barter[r.Member] = b
    end
    b.Money, b.S, b.T = b.Money or m.Money, b.S or 0, b.T or 0
    if r.Op == "sell" then
        for _, it in ipairs(type(m.Items) == "table" and m.Items or {}) do
            if tostring(it.Guid) == tostring(r.Guid) then
                r.Wow = ((M.Item(it.Entry) or {}).sp or 0) * ((r.Count or 0) > 0 and r.Count or it.Count or 1)
            end
        end
        b.S = b.S + (r.Wow or 0)
    else
        for _, w in ipairs((lastInv.Vendors or {})[r.Npc] or {}) do
            if w.Entry == r.Entry then
                r.Wow = r.Op == "buy" and (w.Price or 0) * (r.Count or 1)
                    or math.ceil((w.Price or 0) / math.max(1, r.Lot or 1)) * (r.Count or 1)
            end
        end
        b.T = b.T + (r.Wow or 0)
    end
end
-- A character's gold set to an amount through its own gold stack (BG3's LOOT_Gold_A item): AddGold
-- ignores a negative amount and sometimes a small positive one, UserAddGold works on the whole
-- party's gold (harness), the stack amount is exact and per inventory.
local GOLD = "1c3c9c74-34a1-4685-989e-410dc080be6f"
local function setGold(u, want)
    local it = q(Osi.GetItemByTemplateInInventory, GOLD, u)
    if it then
        if want <= 0 then return pcall(Osi.RequestDelete, it) end
        return pcall(Osi.SetStackAmount, it, want)
    end
    if want > 0 then return pcall(Osi.AddGold, u, want) end
end
local function addGold(u, delta) return setGold(u, goldOf(u) + delta) end
local goldOwed, goldTried = {}, {} -- member -> gold BG3 is short of WoW's money, not yet given back; the last try at it
M.AddGold, M.SetGold = addGold, setGold

-- Stackable things (row kind "object": potions, food, cloth, quest items...) are one BG3 stack per
-- member and entry holding WoW's total (tag "S<entry>@<member>"): BG3 merges stacks of the same item
-- by itself (harness: looted Linen Cloth merged into the bag's stack and its own entity was
-- gone), so a BG3 item per WoW guid cannot hold for them. Equipment stays one BG3 item per WoW item.
local function stackTag(entry, member) return "S" .. tostring(entry) .. "@" .. member end
M.SideTag = {}   -- uuid -> vendor/loot tag, kept here too (a merged stack loses its entity)
M.Refused = {}   -- uuid -> true: a buy or take WoW refused (goes back to the stand-in)
local function entryOf(u)
    local e = q(Ext.Entity.Get, u)
    local v = e and q(function() return e.Vars[ENTRY_VAR] end)
    if math.type(v) == "integer" then return v end
    local sid = e and q(function() return e.Data.StatsId end)
    return type(sid) == "string" and tonumber(sid:match("^" .. PREFIX_STATS .. "(%d+)$")) or nil
end
local stackWant = {}

-- A merchant's: in a vendor's stand-in, or a ware (tag V) on its stand-in or on nobody yet. Everything else,
-- a ware just bought included, is priced as the party's: selling it back pays WoW's sell price.
local function isWare(holder, tag)
    if holder and vendorOf[holder] then return true end
    if tostring(tag):sub(1, 1) ~= "V" then return false end
    return not holder or (WOWBG3MirrorOwned or {})[holder] ~= nil
end
M.IsWare = isWare
local wowCount = {} -- WoW item guid -> what its stack holds (reconcile)
local function priceItem(u, holder, tag)
    local e = q(Ext.Entity.Get, u)
    local entry = e and entryOf(u)
    local row = entry and M.Item(entry)
    if row then setValue(e, M.ValueOf(row, isWare(holder, tag), row.s == "Thrown" and wowCount[tag] or nil)) end
end
-- Every item of ours, by where it is now: after a load, a merge of a ware into a stack, a change of difficulty.
function M.SetPrices()
    M.TradeBase()
    state.TradeBase = tradeBase
    eachOurs(function(u, tag) priceItem(u, holderOf(u), tag) end)
end

-- Food and drink spent in BG3 (switch `camp`): eaten, or taken by BG3 for a long rest. WoW is the truth,
-- so without this the next reconcile put them back and every rest was free. A drop of a BG3 stack below
-- what it last held goes to WoW as the bridge's `del` (WoW's stacks of that entry, bags before anything
-- else); until WoW's list shows it, the stack stays at the lower amount (SPEND_WAIT ms, then WoW's word).
local stackLast, stackSpent = {}, {} -- tag -> amount last held; {Total, N, At}
local pieceOwner = {} -- a part split off a stack (notePieces) -> the member whose stack it came from
local SPEND_WAIT = 15000
local dropSeen, DROP_WAIT = {}, 150 -- tag -> tick a drop was first seen; ticks it must last (about 5 s)
-- Nobody drinks during a long rest, so until this tick a potion's drop is no spending and the old rule below puts it
-- back: a net under the fix for the wipe at a rest's end (M.Reconcile, listedAt). Started to Finished is about 8 s.
local restGuard, REST_START, REST_AFTER = 0, 1800, 900
M.Spent = 0
local function spend(uuid, entry, n, items)
    local left = n
    for _, it in ipairs(items or {}) do
        if left <= 0 then break end
        if it.Entry == entry then
            local c = math.min(left, it.Count or 1)
            M.Request({Op = "del", Member = uuid, Guid = tostring(it.Guid), Count = c})
            left = left - c
        end
    end
    M.Spent = M.Spent + (n - left)
end
-- What is split off an entry's stacks right now: in the barter columns, or on its way out of a hand.
local function piecesOf(entry)
    local n = 0
    for it in pairs(pieceOwner) do
        if not q(Ext.Entity.Get, it) then pieceOwner[it] = nil
        elseif entryOf(it) == entry then n = n + (q(Osi.GetStackAmount, it) or 0) end
    end
    return n
end
-- How far the party's BG3 stacks of this entry together are below what they last held.
local function partyDrop(entry)
    local held, want = piecesOf(entry), 0
    eachOurs(function(u)
        local t = tagOf(u)
        if entryOf(u) == entry and not (t and t:match("^[LV]")) then held = held + (q(Osi.GetStackAmount, u) or 0) end
    end)
    local prefix = "S" .. tostring(entry) .. "@"
    for tag, n in pairs(stackLast) do
        if tag:sub(1, #prefix) == prefix then want = want + n end
    end
    return want - held
end
local function spentCheck(tag, total)
    local p = stackSpent[tag]
    if not p then return total end
    if total < p.Total or Ext.Utils.MonotonicTime() - p.At > SPEND_WAIT then
        stackSpent[tag] = nil
        return total
    end
    return math.max(0, total - p.N)
end

local function reconcileStacks(uuid, stacks, have, items)
    -- An entry WoW no longer lists (sold, used up) starts over: a later purchase is no spending.
    for tag in pairs(stackLast) do
        local e, member = tag:match("^S(%d+)@(.+)$")
        if member == uuid and not stacks[math.tointeger(tonumber(e))] then
            stackLast[tag], stackSpent[tag] = nil, nil
        end
    end
    for entry, total in pairs(stacks) do
        local tag = stackTag(entry, uuid)
        stackWant[tag] = true
        local u = have[tag]
        -- What BG3 uses up is gone from WoW too: camp supplies, and potions and the like (`us`, wow_items.py).
        local row = M.Item(entry) or {}
        local supply = (Camp.On() and row.sv ~= nil) or row.us ~= nil
        local waiting = false
        if supply then
            local wow = total
            total = spentCheck(tag, wow)
            -- Only from a stack seen before: right after a load the save and WoW's snapshot may differ by
            -- anything, and the old rule below settles that without taking food from WoW.
            local was = stackLast[tag] and math.min(stackLast[tag], total) or nil
            local now = was and (u and q(Osi.GetStackAmount, u) or (not u and 0)) or nil
            -- Only what left the party is spent: a part split off or moved to another member or a container
            -- still counts (those the old rule below puts back; WoW's items are never destroyed for them).
            local gone = (now and now < was and not (u and pendingItem[u])) and math.min(was - now, partyDrop(entry)) or 0
            if row.us ~= nil and tick < restGuard then gone = 0 end
            -- Spent only if it stays gone: a stack that vanished for one pass must not cost WoW's food. Until then
            -- it is left as it is.
            if gone > 0 and not (dropSeen[tag] and tick - dropSeen[tag] >= DROP_WAIT) then
                dropSeen[tag] = dropSeen[tag] or tick
                waiting = true
                total = wow - gone
            elseif gone > 0 then
                dropSeen[tag] = nil
                spend(uuid, entry, gone, items)
                stackSpent[tag] = {Total = wow, N = gone, At = Ext.Utils.MonotonicTime()}
                total = wow - gone
            else
                dropSeen[tag] = nil
                -- All of the drop is a piece still out: neither spent nor put back until it is settled.
                waiting = now ~= nil and was ~= nil and now < was and not (u and pendingItem[u]) and piecesOf(entry) > 0
            end
            stackLast[tag] = waiting and was or total
        end
        if not u then
            -- A bought or looted item (vendor/loot tag) the player moved here: it becomes the stack.
            for t, x in pairs(have) do
                if not u and t:match("^[LV]") and not pendingItem[x] and not M.Refused[x] and holderOf(x) == uuid
                    and entryOf(x) == entry then
                    local e = q(Ext.Entity.Get, x)
                    if e and pcall(function() e.Vars[VAR] = tag end) then
                        have[t], have[tag], u = nil, x, x
                    end
                end
            end
        end
        if not u and total > 0 and not waiting then
            local ok, made = pcall(M.Create, entry, uuid, total, tag)
            if ok then u = made; have[tag] = u; state.Created = state.Created + 1 else note(made) end
        end
        if u and not pendingItem[u] and not waiting then
            if holderOf(u) ~= uuid then M.Suppress(u); pcall(Osi.ToInventory, u, uuid, total, 0, 0) end
            local amount = q(Osi.GetStackAmount, u)
            if amount and amount ~= total then pcall(Osi.SetStackAmount, u, total) end
        end
    end
end

-- A party member's own clothes (a playtest request: a companion that joins, an origin hero, keeps
-- what it came in): a WoW starter piece (quality 0 or 1, item level 5 or less) is not put on in BG3
-- over a BG3 piece of the member's own (no WoW tag) in that slot. WoW still wears it; BG3 keeps its
-- look until the WoW character wears something better, which is put on as before.
local OWN_SLOT = {Feet = "Boots", Hands = "Gloves", Chest = "Breast", Robe = "Breast", Head = "Helmet", Back = "Cloak"}
local function keepsOwn(uuid, row)
    local slot = OWN_SLOT[row.s or ""]
    if not slot or (row.q or 0) > 1 or (row.il or 0) > 5 then return false end
    local cur = uuidOf(q(Osi.GetEquippedItem, uuid, slot))
    return cur ~= nil and tagOf(cur) == nil
end
M.KeepsOwn = keepsOwn

-- A WoW thrown weapon is a BG3 dagger: BG3 throws it from the bag or a hand, never from its ranged slot
-- (Throw_Throw's ThrowableTargetConditions). WoW wears it in its ranged slot, beside its main hand, and BG3 has
-- one main hand for both: while WoW holds something there, that goes in BG3's, and the dagger waits in the bag.
function M.ThrownWaits(row, list)
    if row.s ~= "Thrown" then return false end
    for _, it in ipairs(list) do
        if it.Bag == 255 and it.Slot == 15 then return true end
    end
    return false
end
-- WoW's hand slots in BG3. What WoW puts there may push a worn dagger out of BG3's hand: WoW's ranged slot stays.
local BG3_HAND = {[15] = "MeleeMainHand", [16] = "MeleeOffHand"}
local function freeHand(uuid, slot)
    local held = BG3_HAND[slot] and uuidOf(q(Osi.GetEquippedItem, uuid, BG3_HAND[slot]))
    if held and (M.Item(entryOf(held) or -1) or {}).s == "Thrown" then M.Suppress(held) end
end

local function reconcileMember(uuid, m, have)
    if type(m) ~= "table" or m.Error or type(m.Items) ~= "table" then return end
    if not q(Ext.Entity.Get, uuid) then return end
    local stacks = {}
    for _, it in ipairs(m.Items) do
        local tag = tostring(it.Guid)
        wowOwner[tag] = uuid
        local u = have[tag]
        local wantEquip = it.Bag == 255 and it.Slot < 19
        local row = M.Item(it.Entry)
        wowCount[tag] = it.Count
        if row and row.k == "object" then
            stacks[it.Entry] = (stacks[it.Entry] or 0) + (it.Count or 1)
            row = nil -- handled as a stack below
        end
        if row and not u then
            local ok, made = pcall(M.Create, it.Entry, uuid, it.Count, tag, it.Rand)
            if ok then
                u = made
                have[tag] = u
                state.Created = state.Created + 1
            else
                note(made)
            end
        end
        -- A thrown dagger lies where it landed until BG3 picks it up; WoW still has the stack.
        local landed = u and row and row.s == "Thrown" and not holderOf(u) and not equipped(u)
        if u and row and not pendingItem[u] and not landed then
            if it.Rand and M.Props and M.Props[it.Rand] and suffixed[u] ~= it.Rand then
                suffixed[u] = it.Rand -- a bought or looted item learns its suffix here; after a load too
                local v = M.Variant(row, it.Rand)
                pcall(M.MakeStrings, v)
                pcall(M.Dress, u, v)
            end
            if holderOf(u) ~= uuid and not equipped(u) then
                M.Suppress(u)
                pcall(Osi.ToInventory, u, uuid, M.AmountOf(row, it.Count) or 1, 0, 0)
            end
            local eq = equipped(u)
            if wantEquip and not eq and (row.k == "weapon" or row.k == "armor") and not keepsOwn(uuid, row)
                and not M.ThrownWaits(row, m.Items) then
                freeHand(uuid, it.Slot)
                M.Suppress(u)
                pcall(Osi.Equip, uuid, u)
            elseif not wantEquip and eq then
                M.Suppress(u)
                pcall(Osi.Unequip, uuid, u)
            end
            local amount = q(Osi.GetStackAmount, u)
            local want = M.AmountOf(row, it.Count)
            if amount and want and amount ~= want then pcall(Osi.SetStackAmount, u, want) end
        end
    end
    reconcileStacks(uuid, stacks, have, m.Items)
    -- WoW-only pieces the WoW character wears (no BG3 slot): their boosts on the member itself.
    local worn = {}
    for _, it in ipairs(m.Items) do
        local row = M.Item(it.Entry)
        if row and row.k == "worn" and it.Bag == 255 and it.Slot < 19 and row.bo then worn[#worn + 1] = row.bo end
    end
    for _, b in ipairs(M.EffectBoosts(m.Items)) do worn[#worn + 1] = b end
    table.sort(worn)
    local want = table.concat(worn, ";")
    local e = Ext.Entity.Get(uuid)
    local had = q(function() return e.Vars[WORN_VAR] end) or ""
    -- A load brings the variable back but not the boosts (harness): the first look goes by the member.
    if not wornLooked[uuid] then
        wornLooked[uuid] = true
        if not hasWornBoost(e) then had = "" end
    end
    if want ~= had then
        -- Only the boosts that changed: BG3 takes removals out later, so a boost removed and added
        -- again in one tick is lost (a second item's boost cost the first its own).
        local add = {}
        for b in want:gmatch("[^;]+") do add[b] = (add[b] or 0) + 1 end
        for b in had:gmatch("[^;]+") do
            if (add[b] or 0) > 0 then add[b] = add[b] - 1
            else pcall(Osi.RemoveBoosts, uuid, b, 0, "WowBg3Worn", uuid) end
        end
        for b, n in pairs(add) do
            for _ = 1, n do pcall(Osi.AddBoosts, uuid, b, "WowBg3Worn", uuid) end
        end
        pcall(function() e.Vars[WORN_VAR] = want end)
    end
    -- Gold: WoW's money, 1 BG3 gold per CopperPerGold copper (the member's own inventory).
    if type(m.Money) == "number" then
        local want = math.floor(m.Money / (M.CopperPerGold or 1))
        local have_g = goldOf(uuid)
        -- After a barter the price seen is the price paid: BG3's gold, with its attitude and Persuasion, is the
        -- truth. WoW's buys and sells run first (its stock and refusals), then WoW's money is made BG3's. What WoW
        -- refused (onReplies) is taken out at BG3's price, for the member and the merchant both; the rest stands.
        local b = M.Barter[uuid]
        if b and tick - b.At > BARTER_WAIT then
            M.Barter[uuid], b = nil, nil
        end
        -- Any deal in a WoW vendor's trade is a barter, whatever changed hands: BG3's own items (a scroll from the
        -- save, a buyback, the special arrows) have no WoW item to sell or buy, and only the merchant's purse shows
        -- the deal. Read in this same look as the member's gold, so WoW's money never comes back over it (0.1.3013.58:
        -- a BG3 scroll sold for 40, the purse 13100 to 13060, and the next look put WoW's 0 back on Tav).
        if M.Dealt(uuid) and not b then
            b = {At = tick}
            M.Barter[uuid] = b
        end
        if b then
            local busy = false
            for _, r in pairs(pending) do
                if r.Member == uuid and BARTER_OPS[r.Op] then busy = true end
            end
            if not busy then
                -- What BG3 still owes from before (below) is no part of this barter's price.
                if b.Refused and not b.Target then
                    local cpg = M.CopperPerGold or 1
                    local adj = Prices.Refused((have_g + (goldOwed[uuid] or 0)) - (b.Money or m.Money) / cpg, (b.S or 0) / cpg,
                        (b.T or 0) / cpg, (b.RS or 0) / cpg, (b.RT or 0) / cpg)
                    b.Target = have_g + adj
                    M.Refunded(b.Refused, adj)
                end
                local delta = math.floor(((b.Target or have_g) + (goldOwed[uuid] or 0)) * (M.CopperPerGold or 1)) - m.Money
                if delta == 0 then
                    M.Barter[uuid], b = nil, nil
                elseif b.Asked ~= delta then
                    b.Asked = delta
                    M.Request({Op = "money", Member = uuid, Delta = delta})
                end
            end
            if b then want = b.Target or have_g end
        else
            goldOwed[uuid] = want - have_g
        end
        if want ~= have_g then
            M.SuppressGold = tick + 20
            -- BG3 holds a trader's gold stack while a trade is open, so setting it does nothing until the trade
            -- ends (harness: GetGold also lags a tick, so that shows only at the next look). AddGold still
            -- works, upward: a refused buy's price comes back at once.
            local t = goldTried[uuid]
            if t and t.Want == want and t.Have == have_g and want > have_g and not t.Added then
                t.Added = true
                pcall(Osi.AddGold, uuid, want - have_g)
            else
                if not (t and t.Want == want and t.Have == have_g) then goldTried[uuid] = {Want = want, Have = have_g} end
                setGold(uuid, want)
            end
        else
            goldTried[uuid] = nil
        end
    end
end

local function eachHeld(u, fn)
    local e = q(Ext.Entity.Get, u)
    local okI, list = pcall(function() return e.InventoryOwner.PrimaryInventory.InventoryContainer.Items end)
    for _, v in pairs(okI and list or {}) do
        local it = q(function() return v.Item.Uuid.EntityUuid end)
        if it then fn(it) end
    end
end
-- What builds before the stack fix (M.Create) left in a save: bare copies of a stack's food, no tag, WoW's list
-- does not know them. Right after a load nothing can be a live piece yet, so an untagged item of ours is one of
-- those (BG3's rest took it and the stack with it). A copy comes back from a save with no stats entry at all and
-- BG3's name for the food base ("Butter Bun", harness); no BG3 food has no stats. WoW's count stands, nothing is spent.
local swept = {}
local function sweepStrays(uuid)
    local function bare(it)
        local e = q(Ext.Entity.Get, it)
        return q(function() return e.Data.StatsId end) == "" and tostring(q(Osi.GetTemplate, it)):find("^CONS_") ~= nil
    end
    local function look(it)
        if not tagOf(it) and (entryOf(it) or bare(it)) then
            M.Suppress(it)
            if pcall(Osi.RequestDelete, it) then M.Strays = (M.Strays or 0) + 1 end
        end
    end
    eachHeld(uuid, function(it)
        look(it)
        eachHeld(it, look) -- the Camp Supply Sack holds the food
    end)
end

local listedAt, LISTED_WAIT = {}, 900 -- member -> tick of its last list; ticks it may be missing (about 30 s)
function M.Reconcile(inv)
    for uuid, m in pairs(inv.Members or {}) do
        if not swept[uuid] and type(m) == "table" and type(m.Items) == "table" then
            swept[uuid] = true
            pcall(sweepStrays, uuid)
        end
    end
    local have = ours()
    wowOwner, stackWant = {}, {}
    local blind = false
    for uuid, m in pairs(inv.Members or {}) do
        reconcileMember(uuid, m, have)
        if type(m) ~= "table" or type(m.Items) ~= "table" then blind = true else listedAt[uuid] = tick end
    end
    -- A member just listed and now not: at a long rest's end the hero left the list for a moment (hero.json).
    for uuid, at in pairs(listedAt) do
        if not (inv.Members or {})[uuid] then
            if tick - at < LISTED_WAIT then blind = true else listedAt[uuid] = nil end
        end
    end
    -- What WoW no longer has: plain WoW items (guid tags) not in any member's list, stacks it has none of.
    -- Not while a member's list is missing (WoW offline, a far move, the above): that says nothing about what it
    -- holds. What goes here was not spent, so a stack that comes back later starts over.
    for tag, u in pairs(blind and {} or have) do
        if ((isWowGuid(tag) and not wowOwner[tag]) or (tag:match("^S") and not stackWant[tag])) and not pendingItem[u] then
            M.Suppress(u)
            stackLast[tag], stackSpent[tag], dropSeen[tag] = nil, nil, nil
            pcall(Osi.RequestDelete, u)
        end
    end
    state.Members = {}
    for uuid, m in pairs(inv.Members or {}) do
        state.Members[uuid] = {Char = m.Char, Money = m.Money, Items = type(m.Items) == "table" and #m.Items or nil,
            Gold = goldOf(uuid), Error = m.Error}
    end
end

-- WoW's own words for an equip it refuses (InventoryResult, 1.12's GlobalStrings), shown to the player
-- as a BG3 notification (a playtest report: a stolen throwing knife went back into the bag with no word
-- why; WoW said inv_8, no proficiency for thrown weapons).
local EQUIP_WHY = {
    [1] = "You must reach a higher level to use that item.",
    [2] = "You aren't skilled enough to use that item.",
    [3] = "That item doesn't go in that slot.",
    [8] = "You do not have the required proficiency for that item.",
    [9] = "No equipment slot is available for that item.",
    [11] = "You can never use that item.",
    [15] = "You can't equip that while in combat.",
    [17] = "Your race can't use that item.",
}
M.EquipWhy = EQUIP_WHY
local function tellRefused(r, rep)
    if r.Op ~= "equip" or not r.Member then return end
    local code = tonumber(tostring(rep.Why or ""):match("inv_(%d+)"))
    local text = EQUIP_WHY[code or -1] or ("WoW won't equip that (" .. tostring(rep.Why) .. ").")
    local ok = pcall(Osi.ShowNotification, r.Member, text)
    state.LastRefusal = {Text = text, Shown = ok}
end

-- ---- what each vendor has left since the last long rest (the rule: Stock.lua) ----
-- In BG3's save, on every player (WowSave.lua's way): a load brings back that save's counts with nothing to keep in
-- step, and it needs no word from WoW, whose vendors are not in a WoW snapshot and whose counts live in memory.
local STOCK_VAR = "WowBg3Stock"
pcall(Ext.Vars.RegisterUserVariable, STOCK_VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})
local ledger, ledgerSaved = nil, nil -- nil until the party is there to read it from
local function players()
    local out = {}
    for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
        local u = uuidOf(row[1])
        if u then out[#out + 1] = u end
    end
    return out
end
local function readLedger()
    local list, best = players(), nil
    for _, u in ipairs(list) do
        local e = q(Ext.Entity.Get, u)
        local raw = e and q(function() return e.Vars[STOCK_VAR] end)
        local ok, l = pcall(Ext.Json.Parse, type(raw) == "string" and raw or "")
        if ok and type(l) == "table" and type(l.V) == "table" and (not best or (l.N or 0) > (best.N or 0)) then best = l end
    end
    if #list == 0 then return nil end
    best = best or Stock.New()
    best.N = best.N or 0
    ledgerSaved = best.N
    return best
end
local function saveLedger()
    if not ledger or ledger.N == ledgerSaved then return end
    ledgerSaved = ledger.N
    local raw = Ext.Json.Stringify(ledger)
    for _, u in ipairs(players()) do
        local e = q(Ext.Entity.Get, u)
        if e then pcall(function() e.Vars[STOCK_VAR] = raw end) end
    end
end
local STOCK_OPS = {buy = true, buypart = true, add = true}
local function noteSold(r)
    if not (ledger and STOCK_OPS[r.Op] and r.Npc and r.Entry) then return end
    local units = r.Count or 1
    if r.Op == "buy" then units = units * ((M.Item(r.Entry) or {}).bc or 1) end
    Stock.Bought(ledger, tostring(r.Npc), r.Entry, units)
    saveLedger()
end

-- Replies that name a new WoW item for a BG3 item we already made (bought, looted, given): retag it.
local function onReplies(inv)
    for _, rep in ipairs(inv.Replies or {}) do
        local r = pending[rep.Seq]
        if r and rep.RunId == runId then
            pending[rep.Seq] = nil
            if r.Item then pendingItem[r.Item] = nil end
            state.LastReply = rep
            if rep.Ok then noteSold(r) end
            local row = r.Item and M.Item(entryOf(r.Item) or -1)
            if rep.Ok and r.Item and rep.Guid and row and row.k ~= "object" then
                local e = q(Ext.Entity.Get, r.Item)
                if e then pcall(function() e.Vars[VAR] = tostring(rep.Guid) end) end
            end
            if not rep.Ok then
                if r.Item and (r.Op == "buy" or r.Op == "buypart" or r.Op == "take") then M.Refused[r.Item] = true end
                local b = BARTER_OPS[r.Op] and M.Barter[r.Member]
                if b then
                    b.Refused = tostring(r.Npc)
                    if r.Op == "sell" then b.RS = (b.RS or 0) + (r.Wow or 0) else b.RT = (b.RT or 0) + (r.Wow or 0) end
                end
                pcall(tellRefused, r, rep)
                note(string.format("%s refused: %s", tostring(r.Op), tostring(rep.Why)))
            end
        end
    end
end

-- ---- vendors and corpses: their WoW wares or loot in the stand-in's BG3 inventory ----
local sideUuid, sideCount, deleted = {}, {}, {} -- tag -> the stand-in item made for it; its count; ours deleted
local asked = {} -- uuid -> true once a buy or take went out for it
-- Vendor stand-in -> the tick from which a ware leaving it is a buy: its trade screen is open and has settled.
-- Nothing else is one. A world move takes a stand-in apart and opening a trade clears its stock, and every ware
-- "left" it (harness: all of Farley's bought in WoW, then refunded by the barter, each time).
local trading = {}
local TRADE_SETTLE = 45
local function buying(standin) return trading[standin] ~= nil and tick >= trading[standin] end


-- Pickpocketing a stand-in (BG3's RequestPickpocket / StartPickpocket): stand-in uuid -> {Thief, Until}.
-- A ware taken from it then is stolen, not bought: WoW gives the thief the item (the bridge's `add`), and
-- the vendor keeps its stock. Before, it went out as a buy, WoW refused it (no trade, no money), and the
-- next reconcile deleted the item from the thief's bags (a 0.1.0 playtest report).
local picking = {}
local PICK_KEEP = 1800 -- ticks a pickpocketing stays open without a success or failure
-- A ware or loot item that left the stand-in: buy or take it in WoW (once per BG3 item); a ware
-- pickpocketed from it (thief given): the thief gets it from WoW for free.
local function askFor(u, tag, count, thief)
    if asked[u] then return end
    local kind, guid, idx = tostring(tag):match("^([LV])(%x+):(%d+)$")
    if not kind then return end
    asked[u] = true
    local member = thief or Hero.Uuid()
    if kind == "V" then
        local ware = nil
        for _, w in ipairs(((lastInv.Vendors or {})[guid]) or {}) do if tostring(w.Idx) == idx then ware = w end end
        local row = ware and M.Item(ware.Entry) or {}
        local bc = row.bc or 1
        -- One thrown dagger is the whole lot, whatever BG3 says of its amount.
        local n = row.s == "Thrown" and bc or count or 1
        if ware and thief then
            M.Request({Op = "add", Member = member, Entry = ware.Entry, Count = n, Item = u, Npc = guid, Idx = ware.Idx})
            M.Stolen = (M.Stolen or 0) + 1
        elseif ware then
            -- WoW sells a food stack as one purchase (shown as a stack of bc, fillHolder): whole purchases
            -- go as WoW's own buy, the rest of a stack split earlier at the price of one.
            if (ware.Price or 0) > 0 then M.Barter[member] = M.Barter[member] or {At = tick} end
            if n % bc == 0 or (ware.Price or 0) <= 0 then -- price 0: a pocket's food, no vendor's (the bridge refuses it)
                M.Request({Op = "buy", Member = member, Npc = guid, Entry = ware.Entry, Count = n // bc, Item = u})
            else
                M.Request({Op = "buypart", Member = member, Npc = guid, Entry = ware.Entry, Count = n, Price = ware.Price,
                    Lot = bc, Item = u})
            end
        end
    else
        M.Request({Op = "take", Member = member, Npc = guid, Idx = tonumber(idx), Item = u})
    end
end
M.AskFor = askFor
-- Part of a ware bought (split in the drag): BG3 makes the part and moves it in one go, into the buyer's stack if
-- there is one, with no event to see (harness). What shows is the ware's own stack getting smaller, so that is the buy;
-- the part itself goes (notePieces) and WoW's list brings it. A ware BG3 won't stack (arrows) just never gets smaller.
local wareSeen, partBought = {}, {} -- tag -> its stack as last seen in this trade; entry -> tick part of it was bought
local function settleWares()
    local have
    for standin, at in pairs(trading) do
        local guid = vendorOf[standin]
        if guid and tick >= at then
            have = have or ours()
            for _, w in ipairs(((lastInv or {}).Vendors or {})[guid] or {}) do
                local tag = "V" .. guid .. ":" .. tostring(w.Idx)
                local u = have[tag]
                local n = u and not asked[u] and holderOf(u) == standin and q(Osi.GetStackAmount, u)
                if n then
                    local was = wareSeen[tag]
                    if was and n < was and (w.Price or 0) > 0 then
                        local member, bc, part = Hero.Uuid(), (M.Item(w.Entry) or {}).bc or 1, was - n
                        M.Barter[member] = M.Barter[member] or {At = tick}
                        partBought[w.Entry] = tick
                        if part % bc == 0 then
                            M.Request({Op = "buy", Member = member, Npc = guid, Entry = w.Entry, Count = part // bc})
                        else
                            M.Request({Op = "buypart", Member = member, Npc = guid, Entry = w.Entry, Count = part,
                                Price = w.Price, Lot = bc})
                        end
                    end
                    wareSeen[tag] = n
                end
            end
        end
    end
end
M.SettleWares = settleWares


local function fillHolder(standin, prefix, list)
    local have = ours()
    local want, size = {}, {}
    for _, w in ipairs(list or {}) do
        local tag = prefix .. tostring(w.Idx)
        want[tag] = true
        if prefix:sub(1, 1) == "V" then
            local row = M.Item(w.Entry) or {}
            size[tag] = M.AmountOf(row, w.Count or row.bc or 1)
        end
        -- Gone without a trace: the player took it and BG3 merged it into a stack of theirs (no
        -- RemovedFrom for a merge, harness).
        local old = sideUuid[tag]
        if not have[tag] and old and not deleted[old] and not q(Ext.Entity.Get, old) then
            if buying(standin) then askFor(old, tag, sideCount[tag]) else deleted[old] = true end
        end
        if not have[tag] and M.Item(w.Entry) and (not old or asked[old] or deleted[old]) and not (old and pendingItem[old]) then
            local n = M.AmountOf(M.Item(w.Entry), w.Count or (prefix:sub(1, 1) == "V" and M.Item(w.Entry).bc) or 1)
            local ok, made = pcall(M.Create, w.Entry, standin, n, tag)
            if ok then M.SideTag[made] = tag; sideUuid[tag] = made; sideCount[tag] = n else note(made) end
        end
    end
    for tag, u in pairs(have) do
        if tag:sub(1, #prefix) == prefix then
            if not want[tag] then
                -- Sold out, or no longer sold. A ware just bought is the buyer's now (M.Reconcile makes it WoW's).
                local at = holderOf(u)
                if prefix:sub(1, 1) ~= "V" or not at or at == standin then
                    deleted[u] = true
                    M.Suppress(u); pcall(Osi.RequestDelete, u)
                end
            elseif M.Refused[u] and holderOf(u) ~= standin then
                -- WoW said no (money, bags full): back where it came from.
                M.Refused[u], asked[u] = nil, nil
                M.Suppress(u); pcall(Osi.ToInventory, u, standin, q(Osi.GetStackAmount, u) or 1, 0, 0)
            elseif not holderOf(u) and not asked[u] then
                -- Held by nobody: its stand-in was made again (it moved off the ground and back); into
                -- the one there now.
                M.Suppress(u); pcall(Osi.ToInventory, u, standin, 1, 0, 0)
            elseif size[tag] and not buying(standin) and not asked[u] and q(Osi.GetStackAmount, u) ~= size[tag] then
                -- A ware is a full lot until its trade is under way (notePieces took the bare copies, which can
                -- hold part of it); one BG3 won't stack just stays as it is.
                pcall(Osi.SetStackAmount, u, size[tag])
            end
        end
    end
end
local corpseGold = {} -- corpse stand-in -> the gold we put there
-- BG3 won't let a trader buy what it has no gold for ("Insufficient Gold"), and so a merchant holds its purse (Stock.lua)
-- while its trade is open, and nothing after: gold in a stand-in's pockets could be stolen and would become WoW's money.
-- From then on BG3's own barter spends and fills it; the look below keeps what it shows, and WoW's money follows BG3's gold
-- after each deal (the Gold part of reconcileMember), so a purse short of a sale's price pays WoW's money no more than that.
local funded, placed = {}, {} -- stand-in -> the tick of the last try; true once it holds its purse (not set again)
local function levelOf(wares)
    for _, w in ipairs(wares) do
        if w.Level then return w.Level end
    end
end
local function fundMerchant(standin)
    local guid = vendorOf[standin]
    local wares = guid and ledger and ((lastInv or {}).Vendors or {})[guid]
    if not wares or placed[standin] then return end
    local want, have = Stock.PurseOf(ledger, guid, levelOf(wares)), goldOf(standin)
    if have == want then placed[standin] = true; return end
    if tick - (funded[standin] or -1000) < 60 then return end -- GetGold lags, so it is not asked again at once
    funded[standin] = tick
    -- A trade can leave a second pile (trade-prices.md), and setGold sets the first.
    local it = q(Osi.GetItemByTemplateInInventory, GOLD, standin)
    setGold(standin, (it and q(Osi.GetStackAmount, it) or have) + want - have)
end
local purseSeen, traderOf = {}, {} -- stand-in in its trade -> its gold at the last look; the member trading with it
local function lookPurses()
    for standin in pairs(placed) do
        local guid = vendorOf[standin]
        if guid and q(Ext.Entity.Get, standin) then Stock.SetPurse(ledger, guid, goldOf(standin)) end
        if not trading[standin] then placed[standin], funded[standin], purseSeen[standin] = nil, nil, nil end
    end
    saveLedger()
end
M.LookPurses = lookPurses
-- Whether a deal went through since the last look in a trade of this member's: the purse of a merchant it trades with
-- moved. Once the purse is placed, BG3's barter is all that moves it (a refusal's refund comes inside a barter already).
-- Every look reads it, so what moved inside a barter is not read again after it.
function M.Dealt(member)
    local dealt = false
    for standin in pairs(placed) do
        if trading[standin] and (traderOf[standin] or Hero.Uuid()) == member then
            local g = goldOf(standin)
            if purseSeen[standin] and g ~= purseSeen[standin] then dealt = true end
            purseSeen[standin] = g
        end
    end
    return dealt
end

-- BG3's special arrows in place of WoW's arrows and bullets (Stock.lua). BG3's own items in the merchant's bag during its
-- trade, like the purse: no tag and no WoW entry, so the inventory sync does not see them (M.Reconcile, sweepStrays) and
-- BG3's save holds what the party bought. What the bag holds at each look is the stock, so a sale back counts by itself;
-- no WoW item changes hands, and WoW's money follows BG3's gold as after any deal in the trade (M.Dealt).
local arrowsIn = {} -- stand-in -> the tick they went in
local function heroLevel()
    local ok, t = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/wow-level.json") or "")
    return ok and type(t) == "table" and tonumber(t.Level) or nil
end
local function heldArrows(standin)
    local out = {}
    eachHeld(standin, function(it)
        local e = q(Ext.Entity.Get, it)
        local name = e and q(function() return e.Data.StatsId end)
        if Stock.ARROWS[name] then out[name] = (out[name] or 0) + (q(Osi.GetStackAmount, it) or 1) end
    end)
    return out
end
local function putArrows(standin, guid, wares)
    if arrowsIn[standin] or not buying(standin) or not Stock.HasAmmo(wares, M.Item) then return end
    arrowsIn[standin] = tick
    for name, n in pairs(Stock.Arrows(ledger, guid, levelOf(wares), heroLevel())) do
        pcall(Osi.TemplateAddTo, Stock.ARROWS[name][1], standin, n, 0)
    end
    saveLedger()
end
local function lookArrows()
    for standin, at in pairs(arrowsIn) do
        local guid = vendorOf[standin]
        if guid and q(Ext.Entity.Get, standin) and tick - at >= 30 then -- BG3 has them in the bag by then
            local now, was, changed = heldArrows(standin), Stock.Arrows(ledger, guid), false
            for name, n in pairs(now) do changed = changed or was[name] ~= n end
            for name, n in pairs(was) do changed = changed or (n > 0 and not now[name]) end
            if changed then Stock.SetArrows(ledger, guid, now) end
        end
        if not trading[standin] then
            arrowsIn[standin] = nil
            eachHeld(standin, function(it)
                local e = q(Ext.Entity.Get, it)
                if Stock.ARROWS[e and q(function() return e.Data.StatsId end)] then pcall(Osi.RequestDelete, it) end
            end)
        end
    end
    saveLedger()
end
M.LookArrows = lookArrows
-- Every special arrow the party or a merchant holds is worth its value: a load gives them BG3's own again.
local function priceArrows()
    local function look(it)
        local e = q(Ext.Entity.Get, it)
        local v = Stock.ArrowValue(e and q(function() return e.Data.StatsId end))
        if v then setValue(e, v) end
    end
    local function all(holder) eachHeld(holder, function(it) look(it); eachHeld(it, look) end) end
    for uuid in pairs((lastInv or {}).Members or {}) do all(uuid) end
    for standin in pairs(arrowsIn) do all(standin) end
end
M.PriceArrows = priceArrows
-- What WoW refused of a deal goes off the member's gold (the Gold part of reconcileMember). The merchant's purse goes the
-- other way, or a refused buy (a full bag) would fill it for nothing.
function M.Refunded(guid, delta)
    if delta == 0 or not ledger then return end
    for standin in pairs(placed) do
        if vendorOf[standin] == guid then
            local g = math.max(0, goldOf(standin) - delta)
            setGold(standin, g)
            if purseSeen[standin] then purseSeen[standin] = g end -- ours, no deal (M.Dealt)
            return
        end
    end
    local wares = ((lastInv or {}).Vendors or {})[guid] or {}
    Stock.SetPurse(ledger, guid, math.max(0, Stock.PurseOf(ledger, guid, levelOf(wares)) - delta))
    saveLedger()
end

-- The long rest: BG3's traders restock at PROC_LongRest (_Trade.txt), which BG3's long rest runs when the party wakes
-- (GLO_Camp, PROC_Camp_EveryoneAwake), and camp off's refill (Party.lua) stands in for it. It is done once no vendor's
-- trade is open (Stock.Due): a stand-in still in its trade would write its old purse and arrows into the new day.
local function tradeOpen()
    for _, t in ipairs({trading, placed, arrowsIn}) do
        for standin in pairs(t) do
            if q(Ext.Entity.Get, standin) then return true end
        end
    end
    return false
end
local function settleRest()
    if not (ledger and ledger.D) or tradeOpen() then return end
    Stock.Rest(ledger)
    saveLedger()
    state.Restocks = (state.Restocks or 0) + 1
end
M.SettleRest = settleRest
function M.Restock()
    if not ledger then return end
    Stock.Due(ledger)
    settleRest()
    saveLedger()
end


local function fillVendors(inv)
    if not ledger then return end
    -- Every vendor's stand-in whose wares the bridge listed ahead (it lists every friendly NPC with a
    -- stand-in, server patch 0018) holds them before its trade screen opens.
    for standin, guid in pairs(WOWBG3MirrorOwned or {}) do
        if not vendorOf[standin] and (inv.Vendors or {})[tostring(guid)] then vendorOf[standin] = tostring(guid) end
    end
    for standin in pairs(vendorOf) do
        if not Ext.Entity.Get(standin) then vendorOf[standin] = nil end -- its stand-in is gone
    end
    for standin in pairs(vendorOf) do
        if trading[standin] then fundMerchant(standin)
        elseif not corpseOf[standin] and goldOf(standin) > 0 then setGold(standin, 0) end
    end
    for standin, guid in pairs(vendorOf) do
        local wares = (inv.Vendors or {})[guid]
        if wares then
            fillHolder(standin, "V" .. guid .. ":", Stock.Wares(ledger, guid, wares, M.Item))
            if trading[standin] then putArrows(standin, guid, wares) end
        end
    end
    for standin, guid in pairs(corpseOf) do
        local lt = (inv.Loot or {})[guid]
        if lt then
            fillHolder(standin, "L" .. guid .. ":", lt.Items)
            local g = goldOf(standin)
            if corpseGold[standin] and corpseGold[standin] > 0 and g < corpseGold[standin] and not M.MoneyAsked then
                -- The player took the corpse's gold (BG3's loot window): WoW's money from that corpse.
                M.MoneyAsked = true
                M.Request({Op = "take", Member = Hero.Uuid(), Npc = guid, Idx = "money"})
            elseif (lt.Money or 0) ~= g and not M.MoneyAsked then
                setGold(standin, lt.Money or 0)
            end
            corpseGold[standin] = lt.Money or 0
            if (lt.Money or 0) == 0 then M.MoneyAsked = nil end
        end
    end
end

-- Open trade with a WoW vendor (Interact.lua calls this when a click opens BG3's trade screen
-- on a vendor stand-in): asks WoW for its wares; they appear in the stand-in's inventory.
function M.OpenVendor(standin, wowGuid, member)
    standin = uuidOf(standin) or standin
    vendorOf[standin] = tostring(wowGuid)
    pcall(M.NotePieces) -- before the trade counts as open: its strays go
    trading[standin] = tick + TRADE_SETTLE
    traderOf[standin], purseSeen[standin] = uuidOf(member) or member, nil
    pcall(Osi.ClearTradeGeneratedItems, standin)
    fundMerchant(standin)
    M.Request({Op = "vendor", Npc = tostring(wowGuid), Member = member})
end
-- A WoW corpse BG3 lets the player loot (a dead stand-in): asks WoW for its loot.
function M.OpenCorpse(standin, wowGuid, member)
    standin = uuidOf(standin) or standin
    corpseOf[standin] = tostring(wowGuid)
    M.Request({Op = "loot", Npc = tostring(wowGuid), Member = member})
end

-- ---- BG3's moves -> requests ----
local function memberSet(inv)
    local s = {}
    for uuid in pairs((inv or {}).Members or {}) do s[uuid] = true end
    return s
end

local function onEquipped(item, char, on)
    local u, c = uuidOf(item), uuidOf(char)
    if not u or suppressed(u) then return end
    local tag = tagOf(u)
    if not tag or not isWowGuid(tag) then return end
    M.Request({Op = on and "equip" or "unequip", Member = c, Guid = tag, Item = u})
end

-- What the player splits off a WoW stack (dragging part of it into the barter columns) is a new item:
-- our stats, none of our variables, BG3's name and icon. notePieces, on every look, dresses it and says
-- what it came from. It stays a piece until the barter goes through, and only then is it bought or sold
-- (onRemovedFrom, onAddedTo); back on the stack it never happened.
local pieceDressed = {}
local function notePiece(it, row)
    if not pieceDressed[it] then
        pieceDressed[it] = true
        pcall(M.Dress, it, row)
    end
end
-- A piece that has been dealt with: the next reconcile shows the stack as WoW has it.
local function dropPiece(u)
    pieceOwner[u] = nil
    M.Suppress(u)
    pcall(Osi.RequestDelete, u)
end
local function wareTag(guid, entry)
    for _, w in ipairs(((lastInv or {}).Vendors or {})[guid] or {}) do
        if w.Entry == entry then return "V" .. guid .. ":" .. tostring(w.Idx) end
    end
end
local function notePieces()
    for standin, guid in pairs(vendorOf) do
        eachHeld(standin, function(it)
            local entry = not tagOf(it) and entryOf(it)
            local row = entry and M.Item(entry)
            if row and not trading[standin] and not pieceOwner[it] then
                -- No trade, so no part being split off: a bare copy (a load brings them, and BG3 adds it to the
                -- ware's stack on screen: Farley's food showed 10 for 5).
                M.Suppress(it)
                pcall(Osi.RequestDelete, it)
            elseif row then
                notePiece(it, row)
                priceItem(it, standin)
                M.SideTag[it] = wareTag(guid, entry)
            end
        end)
    end
    for uuid in pairs((lastInv or {}).Members or {}) do
        local function look(it)
            local entry = not tagOf(it) and entryOf(it)
            local row = entry and M.Item(entry)
            -- A merchant's piece bought: its buy is on the way, WoW's list brings the stack (a look is what
            -- catches one BG3 put straight in the Camp Supply Sack).
            if row and ((M.SideTag[it] and asked[it]) or (not pieceOwner[it] and tick - (partBought[entry] or -1000) < 90)) then
                dropPiece(it)
            elseif row then notePiece(it, row); priceItem(it, uuid); pieceOwner[it] = uuid end
        end
        eachHeld(uuid, function(it)
            look(it)
            eachHeld(it, look) -- the Camp Supply Sack holds the food
        end)
    end
end
M.NotePieces = notePieces
-- A piece moved to another member goes back on its stack: read as gone, it would cost WoW's food.
local function returnPiece(u, h)
    local owner, entry = pieceOwner[u] or h, entryOf(u)
    local stack = ours()[stackTag(entry, owner)]
    local amount = stack and q(Osi.GetStackAmount, stack)
    if amount then pcall(Osi.SetStackAmount, stack, amount + (q(Osi.GetStackAmount, u) or 0)) end
    dropPiece(u)
end

local function onAddedTo(item, holder)
    local u, h = uuidOf(item), uuidOf(holder)
    if not u or not h then return end
    local tag = tagOf(u)
    priceItem(u, h, tag) -- at once: a ware just bought can be sold straight back
    if suppressed(u) then return end
    local members = memberSet(lastInv)
    if not tag and entryOf(u) then
        if members[h] then
            -- A merchant's piece comes from the buy. One of the party's stays as it is: it may be offered in the
            -- barter columns (BG3 keeps it in the bag until the deal), or be BG3's Throw, which splits one off just
            -- like this and breaks it where it lands (water, alcohol's surface), or a split the player wanted.
            -- When it is gone it reads as spent; a load sweeps what is left (sweepStrays), WoW's count stands.
            if M.SideTag[u] then dropPiece(u)
            elseif pieceOwner[u] and pieceOwner[u] ~= h then returnPiece(u, pieceOwner[u])
            else pieceOwner[u] = h end
        elseif vendorOf[h] and (pieceOwner[u] or buying(h)) then
            -- Part of a stack sold: that many, out of WoW's stacks of that entry. Split in the drag, no look has seen it.
            local seller = pieceOwner[u] or Hero.Uuid()
            local left = q(Osi.GetStackAmount, u) or 1
            for _, it in ipairs((((lastInv.Members or {})[seller]) or {}).Items or {}) do
                if left > 0 and it.Entry == entryOf(u) then
                    local n = math.min(left, it.Count or 1)
                    M.Barter[seller] = M.Barter[seller] or {At = tick}
                    M.Request({Op = "sell", Member = seller, Npc = vendorOf[h], Guid = tostring(it.Guid), Count = n})
                    left = left - n
                end
            end
            dropPiece(u)
        end
        return
    end
    if not tag then return end
    if isWowGuid(tag) then
        local owner = wowOwner[tag]
        if members[h] and owner and owner ~= h then
            M.Request({Op = "give", Member = owner, To = h, Guid = tag, Item = u})
        elseif vendorOf[h] and owner then
            M.Barter[owner] = M.Barter[owner] or {At = tick}
            M.Request({Op = "sell", Member = owner, Npc = vendorOf[h], Guid = tag, Item = u})
        end
        return
    end
    local entry, owner = tag:match("^S(%d+)@(.+)$")
    if entry and owner ~= h and (members[h] or vendorOf[h]) then
        -- A whole stack to another member (give) or to a vendor (sell): every WoW item of that entry.
        local first = true
        for _, it in ipairs((((lastInv.Members or {})[owner]) or {}).Items or {}) do
            if tostring(it.Entry) == entry then
                local r = members[h] and {Op = "give", Member = owner, To = h, Guid = tostring(it.Guid)}
                    or {Op = "sell", Member = owner, Npc = vendorOf[h], Guid = tostring(it.Guid)}
                if not members[h] then M.Barter[owner] = M.Barter[owner] or {At = tick} end
                if first then r.Item = u; first = false end
                M.Request(r)
            end
        end
    end
end

-- A vendor ware or a corpse's loot leaving the stand-in (BG3's trade or loot window): buy or take.
-- Seen on the way out, since a stack may merge into the player's own and lose its entity.
local function onRemovedFrom(item, holder)
    local u, h = uuidOf(item), uuidOf(holder)
    if not u or not h or suppressed(u) then return end
    local tag = M.SideTag[u] or tagOf(u)
    local kind = tostring(tag):match("^([LV])%x+:%d+$")
    local pick = picking[h]
    if kind == "V" and pick and pick.Until >= tick then
        askFor(u, tag, q(Osi.GetStackAmount, u), pick.Thief)
    elseif (kind == "V" and vendorOf[h] and buying(h)) or (kind == "L" and corpseOf[h]) then
        askFor(u, tag, q(Osi.GetStackAmount, u))
    end
end

local function research(rstate)
    if not Harness.On then return end
    local raw = Ext.IO.LoadFile("wow-bg3/inventory-research-cmd.json")
    local ok, cmd = pcall(Ext.Json.Parse, raw or "")
    if not ok or type(cmd) ~= "table" or cmd.RunId ~= M.BaseRunId or math.type(cmd.Sequence) ~= "integer" or cmd.Sequence <= rstate.seq then return end
    rstate.seq = cmd.Sequence
    local okR, value = pcall(function()
        local src = assert(Ext.IO.LoadFile("wow-bg3/inventory-research.lua"), "no research file")
        local chunk = assert(load(src))
        return chunk()(M)
    end)
    Ext.IO.SaveFile("wow-bg3/inventory-research-result.json", Ext.Json.Stringify({RunId = M.BaseRunId, Sequence = cmd.Sequence,
        OK = okR, Value = okR and value or tostring(value)}))
end

function M.Start(id)
    -- One key per Lua state (bg3le rebuilds the states at every load, request numbers start again).
    runId = tostring(id) .. "-" .. string.format("%x%04x", math.floor(Ext.Utils.MonotonicTime()), math.random(0, 0xffff))
    M.RunKey = runId
    M.BaseRunId = id
    WOWBG3Inventory = M
    Ext.Events.StatsLoaded:Subscribe(function() pcall(M.MakeUsed) end)
    Ext.Events.SessionLoaded:Subscribe(function() M.MakeAll() end)
    if client then return end
    Ext.Osiris.RegisterListener("Equipped", 2, "after", function(item, char) onEquipped(item, char, true) end)
    Ext.Osiris.RegisterListener("Unequipped", 2, "after", function(item, char) onEquipped(item, char, false) end)
    Ext.Osiris.RegisterListener("AddedTo", 3, "after", function(item, holder) onAddedTo(item, holder) end)
    Ext.Osiris.RegisterListener("RemovedFrom", 2, "after", function(item, holder) onRemovedFrom(item, holder) end)
    pcall(Ext.Osiris.RegisterListener, "LongRestStarted", 0, "after", function() restGuard = tick + REST_START end)
    pcall(Ext.Osiris.RegisterListener, "LongRestFinished", 0, "after", function() restGuard = tick + REST_AFTER end)
    -- The restock follows BG3's own traders: one per long rest (it ran on LongRestFinished as well, so twice).
    pcall(Ext.Osiris.RegisterListener, "PROC_LongRest", 0, "after", function() M.Restock() end)
    -- A load in this same Lua state: that save's counts (Hero.lua makes a new token, WowSave.lua starts over the same way).
    Ext.Events.GameStateChanged:Subscribe(function(e)
        -- No trade is open after a load either: one left open would hold a due long rest back (settleRest).
        if tostring(e.ToState) == "LoadSession" then ledger, placed, funded, arrowsIn, purseSeen, trading = nil, {}, {}, {}, {}, {} end
    end)
    -- A WoW mob's stand-in dying in BG3: its WoW loot goes on the BG3 corpse. The server makes the
    -- WoW corpse lootable a moment after the kill reaches it, so it is asked from 15 ticks on and every
    -- 30 ticks until WoW's answer has the loot (CORPSE_ASKS at most). It was asked at 30 and 120 ticks:
    -- the loot came 3 to 4 s after the kill, and a loot window opened before showed an empty corpse.
    local corpseDue = {}
    local CORPSE_ASKS = 8
    -- A stand-in's own BG3 things (its template's gear: the human template's sword and shield) go when it
    -- dies, so its corpse holds the WoW loot alone and BG3's loot sparkle goes once that is taken
    -- (a playtest report: the sparkle stayed on an emptied corpse).
    local function clearOwn(u)
        local e = Ext.Entity.Get(u)
        local okI, list = pcall(function() return e.InventoryOwner.PrimaryInventory.InventoryContainer.Items end)
        for _, v in pairs(okI and list or {}) do
            local it = q(function() return v.Item.Uuid.EntityUuid end)
            if it and not tagOf(it) then M.Suppress(it); pcall(Osi.RequestDelete, it) end
        end
        for _, slot in ipairs({"MeleeMainHand", "MeleeOffHand", "RangedMainHand", "RangedOffHand", "Helmet",
                               "Breast", "Cloak", "Gloves", "Boots", "Amulet", "Ring", "Ring2"}) do
            local it = uuidOf(q(Osi.GetEquippedItem, u, slot))
            if it and not tagOf(it) then M.Suppress(it); pcall(Osi.RequestDelete, it) end
        end
    end
    pcall(Ext.Osiris.RegisterListener, "Died", 1, "after", function(c)
        local u = uuidOf(c)
        local guid = u and (WOWBG3MirrorOwned or {})[u]
        if guid then
            corpseDue[u] = {Guid = tostring(guid), Tick = tick + 15, Left = CORPSE_ASKS}
            pcall(clearOwn, u)
        end
    end)
    -- Pickpocketing a stand-in (see picking): open from the request until a success or failure (a
    -- little after, for the item moves), and gold taken is WoW money for the thief.
    for _, ev in ipairs({{"RequestPickpocket", 2}, {"StartPickpocket", 3}}) do
        pcall(Ext.Osiris.RegisterListener, ev[1], ev[2], "after", function(thief, npc)
            local t, n = uuidOf(thief), uuidOf(npc)
            if t and n and (WOWBG3MirrorOwned or {})[n] then picking[n] = {Thief = t, Until = tick + PICK_KEEP} end
        end)
    end
    pcall(Ext.Osiris.RegisterListener, "CharacterPickpocketFailed", 2, "after", function(_, npc)
        local p = picking[uuidOf(npc) or ""]
        if p then p.Until = tick + 30 end
    end)
    pcall(Ext.Osiris.RegisterListener, "CharacterPickpocketSuccess", 6, "after", function(thief, npc, item, template, amount)
        local t, n = uuidOf(thief), uuidOf(npc)
        if not (t and n and (WOWBG3MirrorOwned or {})[n]) then return end
        local p = picking[n]
        if p then p.Until = math.max(p.Until, tick + 60) end
        if tostring(template or ""):find("LOOT_Gold", 1, true) and (tonumber(amount) or 0) > 0 then
            M.Request({Op = "money", Member = t, Delta = math.floor(tonumber(amount) * (M.CopperPerGold or 1))})
        end
    end)
    pcall(Ext.Osiris.RegisterListener, "TradeEnds", 2, "after", function(a, b)
        for _, x in ipairs({uuidOf(a) or "", uuidOf(b) or ""}) do
            if trading[x] then trading[x] = nil; wareSeen = {} end
        end
    end)
    -- BG3's trade with a WoW vendor's stand-in, however it was opened (interaction's click, the
    -- context menu, dialog): fill it with the WoW vendor's wares.
    for _, ev in ipairs({{"RequestTrade", 4}, {"RequestTrade", 3}}) do
        pcall(Ext.Osiris.RegisterListener, ev[1], ev[2], "after", function(char, trader)
            local t = uuidOf(trader)
            local guid = t and (WOWBG3MirrorOwned or {})[t]
            if guid then M.OpenVendor(t, guid, uuidOf(char)) end
        end)
    end
    M.CorpseDue = corpseDue
    local rstate, lastRaw = {seq = 0}, nil
    -- A research command already in the file is done (it would run again after a load).
    pcall(function()
        local c = Ext.Json.Parse(Ext.IO.LoadFile("wow-bg3/inventory-research-cmd.json") or "")
        if c.RunId == id and math.type(c.Sequence) == "integer" then rstate.seq = c.Sequence end
    end)
    Ext.Events.Tick:Subscribe(function()
        tick = tick + 1
        if tick % 10 ~= 0 or Ext.Utils.GetGameState() ~= "Running" then return end
        if not made then M.MakeAll() end
        pcall(settleWares)
        ledger = ledger or readLedger()
        pcall(lookArrows)
        pcall(lookPurses)
        pcall(settleRest) -- after the looks: a trade that closed has written its last into its own day
        if reqSeq == 0 then M.Request({Op = "list"}) end -- hello: the bridge answers this state from now on
        if not M.Redressed then M.Redressed = M.Redress() end
        for u, f in pairs(M.Fresh) do
            if tick - f.Tick >= 10 then M.Fresh[u] = nil; pcall(M.DressFresh, u, f.Row) end
        end
        if tick % 20 == 0 then
            pcall(M.NotePieces)
            pcall(M.SetPrices)
            pcall(M.PriceArrows)
        end
        for u, t in pairs(pendingItem) do if tick - t > 1800 then pendingItem[u] = nil end end
        for u, d in pairs(M.CorpseDue) do
            local lt = lastInv and (lastInv.Loot or {})[d.Guid]
            if d.Asked and lt and ((lt.Money or 0) > 0 or #(lt.Items or {}) > 0) then
                M.CorpseDue[u] = nil -- WoW's loot is there
            elseif tick >= d.Tick then
                M.OpenCorpse(u, d.Guid, Hero.Uuid())
                d.Asked, d.Left, d.Tick = true, d.Left - 1, tick + 30
                if d.Left <= 0 then M.CorpseDue[u] = nil end
            end
        end
        pcall(research, rstate)
        local raw = Ext.IO.LoadFile(IO .. "inventory.json")
        if raw and raw ~= lastRaw then
            lastRaw = raw
            local ok, inv = pcall(Ext.Json.Parse, raw)
            -- Only a bridge answering this Lua state (inv.For: the RunId of the requests it read), never
            -- a file left from an earlier run or a test.
            if ok and type(inv) == "table" and inv.For == runId and inv.Seq ~= state.Seq then
                state.Seq = inv.Seq
                lastInv = inv
                local okR, err = pcall(function()
                    onReplies(inv)
                    M.Reconcile(inv)
                    fillVendors(inv)
                end)
                if not okR then note(err) end
            end
        end
        if tick % 60 == 0 then
            pcall(M.SaveUsed)
            state.Tick, state.RunId, state.MadeMs, state.ReqSeq = tick, runId, M.MadeMs, reqSeq
            state.Stolen, state.Strays = M.Stolen, M.Strays
            Ext.IO.SaveFile(IO .. "inventory-status.json", Ext.Json.Stringify(state))
        end
    end)
end

return M
