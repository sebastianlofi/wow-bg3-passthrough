-- Progression (server; the progression notes): BG3 levels the WoW way.
--
-- The design: the hero's WoW levels 1 to 4 each open a party slot (level 1: the hero alone); every WoW level
-- from 5 on earns one level-up for one member, every 5th two (M.CURVE): 4 members x BG3 level 18 at WoW 60.
-- BG3 itself stops at 12 without a level-cap mod, and the rest waits in the pool.
-- One command (a playtest request): the WoW level, the only one. While the pool holds a level-up,
-- BG3's own Level Up button shows on every member who has room (M.Offer: its next level's XP); the
-- player levels whichever one they want, BG3's LeveledUp spends the level-up on that one, and once the
-- pool is empty the others' buttons go (their XP back to their level's). `levelup NAME` is gone.
--
-- How (BG3's own mechanisms only):
--   * BG3's own XP is off: every ExperienceReward resource (combat XP of a kill by the victim's stats
--     XPReward and level, quest and exploration rewards; Shared/Gustav Levelmaps/ExperienceRewards.lsx)
--     has its PerLevelRewards set to 0 when the session starts. Stand-ins give none, quests none.
--   * A level-up is XP: the member gets exactly the XP for its next level (BG3's ExtraData LevelN, the
--     XP from level N to N+1; a level-cap mod's own table for 13+) with Osiris AddExplorationExperience,
--     so BG3's Level Up button appears and every class choice stays the player's.
--   * Reset is Withers' respec (Osiris StartRespec, BG3's respec screen); when BG3 reports
--     RespecCompleted the member's XP goes back to 0 and its level-ups back to the pool.
--   * The hero's XP bar shows WoW's own progress: the percent into the next WoW level is written into
--     BG3's Experience component between the XP of the hero's BG3 level and one XP short of the next
--     level's, so BG3's bar and tooltip fill while its level and its Level Up button never move
--     (M.ShowWowProgress; the guard below allows exactly that range for the hero).
--   * The pool lives in the save: a persistent user variable on every player character (Ext.Vars, as
--     WowSave.lua; the unpacked harness mod has no PersistentVars), JSON {Rev, WowLevel, Ups = {uuid =
--     level-ups given}}; the newest Rev wins on load.
-- The WoW side only reaches here through wow-bg3/wow-level.json {Character, Level, Percent, Seq} (WoW
-- backend: link/tools/wowlevel.py); nothing below knows WoW's version (wow-backend-contract.md).
local M = {}
local Harness = Ext.Require("Harness.lua")
local Hero = Ext.Require("Hero.lua")

local VAR = "WowBg3Pool"
pcall(Ext.Vars.RegisterUserVariable, VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})

M.SLOT_LEVELS = 4  -- WoW levels 1..4: one party slot each
M.FIRST_EARN = 5   -- every WoW level from here on: one level-up

-- The curve is this one table, a party of four at BG3 level Cap at WoW 60. One level-up per WoW level from 5 and a second at every Every-th, counted back
-- from 60: 56 + 12 = 68 = 4 x 17 for 4x18, so all four can be 17 at WoW 57. 4x17 is this one change:
-- Cap = 17, Every = 7 (64 = 4 x 16). BG3's own party limit follows the slots, and the popups below explain it all.
M.WOW_MAX = 60
M.CURVE = {Cap = 18, Every = 5}
-- Companions at camp and the popups: settings.conf `companions = 1`, the launcher's wow-bg3/companions.json {On}.
local camp = false
function M.CampOn() return camp end
local function readSwitch()
    local ok, v = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/companions.json") or "")
    camp = ok and type(v) == "table" and v.On == true
end

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return Hero.UuidOf(tostring(s)) end
local function nameOf(u) return q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end) or u end
local function log(s) Ext.Utils.Print("WOW-BG3 progression: " .. s) end
-- A refusal for the player: the message alone (no Lua position).
local function need(v, msg)
    if not v then error(msg, 0) end
    return v
end

-- BG3's XP table: ExtraData LevelN is the XP from level N to N+1; MaxXPLevel the highest level XP
-- reaches (12 in the base game; a level-cap mod raises both).
local function extra(k) return q(function() return Ext.Stats.GetStatsManager().ExtraData[k] end) end
function M.MaxXpLevel() return math.floor(tonumber(extra("MaxXPLevel")) or 12) end
function M.XpFor(n)
    local total = 0
    for i = 1, n - 1 do
        local v = tonumber(extra("Level" .. i))
        if not v then return nil end
        total = total + math.floor(v)
    end
    return total
end
local function levelForXp(xp)
    local n = 1
    while n < M.MaxXpLevel() do
        local need = M.XpFor(n + 1)
        if not need or xp < need then break end
        n = n + 1
    end
    return n
end
local function xpOf(u)
    return q(function()
        local e = Ext.Entity.Get(u)
        return {Total = e.Experience.TotalExperience, Level = e.EocLevel.Level,
            Available = e.AvailableLevel and e.AvailableLevel.Level or e.EocLevel.Level}
    end)
end

-- BG3's own XP off (see the header). Static data is one copy per process, kept across loads.
local xpOffDone = false
function M.XpOff()
    local ids = Ext.StaticData.GetAll("ExperienceReward") or {}
    local zeroed, already = 0, 0
    for _, id in ipairs(ids) do
        local r = Ext.StaticData.Get(id, "ExperienceReward")
        local changed = false
        for i = 1, #r.PerLevelRewards do
            if r.PerLevelRewards[i] ~= 0 then r.PerLevelRewards[i] = 0; changed = true end
        end
        if changed then zeroed = zeroed + 1 else already = already + 1 end
    end
    xpOffDone = true
    log(string.format("BG3's own XP off: %d experience rewards zeroed (%d were 0)", zeroed, already))
    return {Zeroed = zeroed, AlreadyZero = already, Total = #ids}
end

-- The pool (see the header).
-- Seen, Queue: companions at camp only (the popups shown, the popups waiting).
local pool = {Rev = 0, WowLevel = nil, Ups = {}, Seen = {}, Queue = {}}
local function players()
    local out = {}
    for _, r in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
        local u = uuidOf(r[1])
        if u then out[#out + 1] = u end
    end
    return out
end
local function loadPool()
    local best = nil
    for _, u in ipairs(players()) do
        local raw = q(function() return Ext.Entity.Get(u).Vars[VAR] end)
        local ok, v = pcall(Ext.Json.Parse, type(raw) == "string" and raw or "")
        if ok and type(v) == "table" and math.type(v.Rev) == "integer" and (not best or v.Rev > best.Rev) then best = v end
    end
    if best then
        pool = {Rev = best.Rev, WowLevel = math.tointeger(best.WowLevel), Ups = type(best.Ups) == "table" and best.Ups or {},
            Seen = type(best.Seen) == "table" and best.Seen or {},
            Queue = type(best.Queue) == "table" and best.Queue or {}}
    else
        pool = {Rev = 0, WowLevel = nil, Ups = {}, Seen = {}, Queue = {}}
    end
    return best ~= nil
end
local function savePool()
    pool.Rev = pool.Rev + 1
    local s = Ext.Json.Stringify({Rev = pool.Rev, WowLevel = pool.WowLevel, Ups = pool.Ups,
        Seen = pool.Seen, Queue = pool.Queue})
    for _, u in ipairs(players()) do
        local e = q(Ext.Entity.Get, u)
        if e then pcall(function() e.Vars[VAR] = s end) end
    end
end

-- The hero's WoW level from the backend (wow-bg3/wow-level.json). A file written before this session
-- started (bootSeq) is not taken: after a load the save's own level stands until the backend reports.
local bootSeq, lastSeq = nil, nil
local progressPct = nil -- how far WoW's XP stands into the hero's next WoW level (0 to 99.9), nil: unknown
local function readLevelFile()
    local raw = Ext.IO.LoadFile("wow-bg3/wow-level.json")
    local ok, v = pcall(Ext.Json.Parse, raw or "")
    if ok and type(v) == "table" and math.type(v.Seq) == "integer" and math.type(v.Level) == "integer" then return v end
end
function M.RefreshWowLevel()
    local v = readLevelFile()
    if not v or v.Seq == bootSeq or v.Seq == lastSeq then return false end
    lastSeq = v.Seq
    progressPct = tonumber(v.Percent)
    if v.Level < 1 then return false end
    if pool.WowLevel ~= v.Level then
        log(string.format("WoW level %s -> %d (%s)", tostring(pool.WowLevel), v.Level, tostring(v.Character)))
        if pool.WowLevel and v.Level > pool.WowLevel then M.ding = true end
        pool.WowLevel = v.Level
        savePool()
    end
    return true
end
function M.WowLevel() return pool.WowLevel or 1 end
function M.Slots() return math.max(1, math.min(M.WowLevel(), M.SLOT_LEVELS)) end
-- Whether this WoW level gives a second level-up.
local function isDouble(l)
    return l >= M.FIRST_EARN and l <= M.WOW_MAX and (M.WOW_MAX - l) % M.CURVE.Every == 0
end
function M.Earned()
    local top, n = math.min(M.WowLevel(), M.WOW_MAX), 0
    for l = M.FIRST_EARN, top do if isDouble(l) then n = n + 1 end end
    return math.max(0, top - M.FIRST_EARN + 1) + n
end
local function grantCap() return M.CURVE.Cap end
-- Whether a member holding `ups` level-ups may take one more by the rules (not BG3's XP table).
local function capRoom(ups) return 2 + ups <= grantCap() end
local function capText(name, at)
    return string.format("%s is at BG3 level %d, the highest the WoW levels give", name, at)
end

-- Level-ups a character holds. A party member the pool has not seen yet (an older save, a companion
-- that joined with levels) keeps the levels it has: its levels above 1 count as spent. Only levels it
-- has taken count, not XP: companions join at BG3 level 1 holding the XP of the hero's level (BG3's
-- own, or the exploration XP an offer gave, shared with the party), which counted as a level-up each
-- (a playtest: WoW level 5 earned one, Tav took it, then Shadowheart, Astarion and Gale joined
-- "with 1 level-up(s) it already had" and showed Level Up). The XP above its level goes back (guard).
local function upsOf(u)
    local n = pool.Ups[u]
    if math.type(n) == "integer" then return n end
    local x = xpOf(u)
    n = x and math.max(0, x.Level - 1) or 0
    pool.Ups[u] = n
    savePool()
    if n > 0 then log(string.format("%s joins the pool with %d level-up(s) it already had", nameOf(u), n)) end
    return n
end
function M.Spent()
    local n = 0
    for _, v in pairs(pool.Ups) do if math.type(v) == "integer" then n = n + v end end
    return n
end
function M.Available() return M.Earned() - M.Spent() end

-- Party slots (Party.lua's Add): nil when one is open, else why not.
function M.AddRefusal(partySize)
    M.RefreshWowLevel()
    if partySize < M.Slots() then return nil end
    if M.Slots() >= M.SLOT_LEVELS then return nil end -- the 4th slot is BG3's own party limit's business
    M.Popup("refused")
    return string.format("no party slot open: WoW level %d opens %d slot(s) (one per WoW level up to %d), the party has %d",
        M.WowLevel(), M.Slots(), M.SLOT_LEVELS, partySize)
end

local function member(u, hero)
    local x = xpOf(u) or {}
    local ups = upsOf(u)
    return {Uuid = u, Name = nameOf(u), Hero = u == hero, LevelUps = ups, Level = x.Level, Available = x.Available,
        Xp = x.Total, Target = 1 + ups, TargetXp = M.XpFor(1 + ups)}
end
function M.Status(party)
    local hero = Hero.Uuid()
    local out = {WowLevel = M.WowLevel(), WowPercent = progressPct, Slots = M.Slots(), Earned = M.Earned(),
        MaxXpLevel = M.MaxXpLevel(), Cap = grantCap(), GrantCap = grantCap(), XpOff = xpOffDone,
        Members = {}, Others = {}, Camp = camp,
        Popups = camp and pool.Queue or nil, WowMax = M.WOW_MAX}
    local inParty = {}
    for _, u in ipairs(party) do
        inParty[u] = true
        out.Members[#out.Members + 1] = member(u, hero)
    end
    out.Spent, out.Available = M.Spent(), M.Available()
    for u, n in pairs(pool.Ups) do
        if not inParty[u] and n > 0 then out.Others[#out.Others + 1] = {Uuid = u, Name = nameOf(u), LevelUps = n} end
    end
    return out
end

-- A member's XP set to `total`, as BG3 keeps it: the Experience component (total, into this level, to
-- the next) and AvailableLevel (the level its XP reaches: BG3 shows Level Up while it is above the
-- character's level and drops its CanLevelUp tag by itself a tick after it is not). BG3 only ever
-- raises XP (AddExplorationExperience with a negative amount leaves AvailableLevel and a negative
-- "into this level"; harness), so lowering goes through the components.
local function setXp(u, total)
    local e = Ext.Entity.Get(u)
    local lvl = levelForXp(total)
    local base, nxt = M.XpFor(lvl) or 0, M.XpFor(lvl + 1)
    e.Experience.TotalExperience = total
    e.Experience.CurrentLevelExperience = total - base
    e.Experience.NextLevelExperience = nxt and (nxt - base) or 0
    e:Replicate("Experience")
    if e.AvailableLevel then
        e.AvailableLevel.Level = math.max(lvl, e.EocLevel.Level)
        e:Replicate("AvailableLevel")
    end
    return lvl
end
M.SetXp = setXp

-- WoW's XP progress in BG3's own XP bar: the hero may hold the XP of its BG3 level plus WoW's percent
-- of the way to its next BG3 level, at most one XP short of it. barRoom is the most it may hold (its
-- own level's XP for everyone else), barXp the value that shows WoW's percent.
local busy = {}
-- Level-ups a command granted (M.LevelUp) that the member has not taken in BG3 yet: uuid -> true. The
-- offers (M.Offer, below): uuid -> true while it holds its next level's XP for the pool.
local granted, offered = {}, {}
local function barRoom(u, ups)
    local target = M.XpFor(1 + ups)
    if not target or not progressPct or u ~= Hero.Uuid() then return target end
    local nxt = M.XpFor(2 + ups)
    if not nxt or nxt <= target + 1 then return target end
    return nxt - 1
end
local function barXp(u, ups)
    local target, room = M.XpFor(1 + ups), barRoom(u, ups)
    if not target or not room or room <= target then return nil end
    local pct = math.max(0, math.min(99.9, progressPct or 0)) / 100
    return math.min(target + math.floor((room + 1 - target) * pct), room)
end

-- The XP a member should have: exactly its level's (1 + level-ups), the hero's bar progress on top (see
-- above). BG3's own XP is off, so only a story script's fixed grant (Act 1's well:
-- AddExplorationExperience 250) can put a member above that; it is taken back (logged). Also a stale
-- AvailableLevel above what the XP reaches. A member counted with more level-ups than the levels it took
-- (and none waiting: not granted, not offered) gives the rest back to the pool (the joins above, in
-- saves from before). (Before, `offered` was declared below this function, so every run of it stopped
-- on a nil index inside its pcall and no XP was ever set back.)
local function guard(party)
    for _, u in ipairs(party) do
        local x = not busy[u] and not M.InRespec(u) and not offered[u] and xpOf(u)
        local ups = x and pool.Ups[u]
        if math.type(ups) == "integer" and not granted[u] and ups > x.Level - 1 then
            pool.Ups[u] = math.max(0, x.Level - 1)
            savePool()
            log(string.format("%s: counted %d level-up(s) at BG3 level %d: %d back to the pool", nameOf(u), ups, x.Level,
                ups - pool.Ups[u]))
            ups = pool.Ups[u]
        end
        local target = ups and M.XpFor(1 + ups)
        local room = target and (barRoom(u, ups) or target)
        if target and (x.Total > room or x.Available > math.max(x.Level, levelForXp(x.Total))) then
            local was = x.Total
            setXp(u, math.min(x.Total, room))
            log(string.format("%s: XP %d is not the WoW progression's (%d for BG3 level %d%s): set back", nameOf(u), was,
                target, 1 + ups, room > target and string.format(", up to %d with WoW's bar progress", room) or ""))
        end
    end
end

-- The hero's own bar: WoW's percent into its next WoW level, written the way XP is lowered (the
-- components, then Replicate). Capped one XP short of the next BG3 level, so BG3's level, its
-- AvailableLevel and its Level Up button never change and nothing is granted by it. Skipped while a
-- level-up waits (BG3's Level Up button open), during a respec, while a grant runs, and when the XP is
-- outside the range this writes (the guard's business then).
local shownPct = {}
function M.ShowWowProgress()
    if not progressPct then return end
    local u = Hero.Uuid()
    if not u or busy[u] or M.InRespec(u) or offered[u] then return end
    local ups = pool.Ups[u]
    if math.type(ups) ~= "integer" then return end
    local target, room, want = M.XpFor(1 + ups), barRoom(u, ups), barXp(u, ups)
    if not target or not room or not want then return end
    local x = xpOf(u)
    if not x or x.Total == want then return end
    if x.Level ~= 1 + ups or x.Available > x.Level then return end
    if x.Total < target or x.Total > room then return end
    setXp(u, want)
    local pct = math.floor(progressPct)
    if shownPct[u] == nil or math.abs(pct - shownPct[u]) >= 10 then
        shownPct[u] = pct
        log(string.format("%s: WoW's XP %d%% into the level shown in BG3's bar (XP %d of %d, BG3 level %d)",
            nameOf(u), pct, want, room + 1, 1 + ups))
    end
end

M.Offered = offered
local function room(u, ups)
    local level = 2 + ups
    return capRoom(ups) and level <= M.MaxXpLevel() and M.XpFor(level) or nil
end
-- The member BG3's Level Up screen is open for (RenderScreen.lua's link/levelupfor.txt: its display-name
-- handle), or nil. While it is open the offer is that member's alone: BG3 goes on from an accepted
-- level-up to the next member who shows Level Up, opened before the pool could withdraw it, and that
-- one took a level-up the pool did not have (a playtest; harness: Tav took the only one, BG3 went
-- on to Shadowheart, the pool went to -1). Closing the screen without levelling brings the offers back.
local function levelUpFor(party)
    local h = (Ext.IO.LoadFile("wow-bg3/link/levelupfor.txt") or ""):match("^%s*(%S+)")
    if not h then return nil end
    for _, u in ipairs(party) do
        if q(Osi.GetDisplayName, u) == h then return u end
    end
    return nil
end
function M.Offer(party)
    M.RefreshWowLevel()
    local avail = M.Available()
    local focus = levelUpFor(party)
    local inParty = {}
    for _, u in ipairs(party) do
        inParty[u] = true
        local x = not busy[u] and not M.InRespec(u) and xpOf(u)
        local ups = x and upsOf(u)
        local want = ups and avail > 0 and (not focus or focus == u) and room(u, ups)
        if x and want and not offered[u] and x.Level == 1 + ups then
            local need = want - x.Total
            offered[u] = true
            if need > 0 then
                local ok, err = pcall(Osi.AddExplorationExperience, u, need)
                if not ok then offered[u] = nil; log("offer failed: " .. tostring(err)) end
            end
            log(string.format("%s: Level Up offered (BG3 level %d, %d level-up(s) in the pool)", nameOf(u), 2 + ups, avail))
        elseif x and offered[u] and not want then
            offered[u] = nil
            setXp(u, M.XpFor(1 + ups) or 0)
            log(string.format("%s: Level Up withdrawn (%s)", nameOf(u), focus and focus ~= u and
                ("BG3's Level Up screen is " .. nameOf(focus) .. "'s") or "the pool is empty"))
        end
    end
    for u in pairs(offered) do if not inParty[u] then offered[u] = nil end end
end
-- BG3's LeveledUp: the level-up the member took is spent on it (its level now, as the pool counts it).
local function leveledUp(c)
    local u = uuidOf(c)
    if not u or busy[u] then return end
    local x = xpOf(u)
    if not x then return end
    local ups = upsOf(u)
    if x.Level - 1 >= ups then granted[u] = nil end
    if x.Level - 1 > ups then
        pool.Ups[u] = x.Level - 1
        offered[u] = nil
        savePool()
        log(string.format("%s: took a level-up (BG3 level %d), %d left in the pool", nameOf(u), x.Level, M.Available()))
    end
end

-- One level-up from the pool to that member (harness; the player's way is BG3's Level Up, M.Offer).
function M.LevelUp(u, wait)
    M.RefreshWowLevel()
    local name = nameOf(u)
    local ups = upsOf(u)
    need(M.Available() > 0, string.format("no level-up to give: WoW level %d earned %d (one per WoW level from %d), %d given",
        M.WowLevel(), M.Earned(), M.FIRST_EARN, M.Spent()))
    local level = 1 + ups + 1
    need(capRoom(ups), capText(name, level - 1))
    need(level <= M.MaxXpLevel(), string.format("BG3 level %d needs a level-cap mod (BG3's XP table ends at %d)", level, M.MaxXpLevel()))
    local target = need(M.XpFor(level), "BG3's XP table has no Level" .. (level - 1))
    local before = xpOf(u) or {Total = 0}
    local others = {}
    for _, o in ipairs(players()) do if o ~= u then others[o] = (xpOf(o) or {}).Total end end
    local need = target - before.Total
    pool.Ups[u] = ups + 1
    granted[u] = true
    busy[u] = true
    if need > 0 then
        local ok, err = pcall(Osi.AddExplorationExperience, u, need)
        if not ok then pool.Ups[u] = ups; granted[u] = nil; busy[u] = nil; error("AddExplorationExperience failed: " .. tostring(err), 0) end
        wait(5)
    end
    busy[u] = nil
    savePool()
    local after = xpOf(u) or {}
    local leaked = {}
    for o, xp in pairs(others) do
        local now = (xpOf(o) or {}).Total
        if now and xp and now ~= xp then leaked[#leaked + 1] = {Name = nameOf(o), Before = xp, After = now} end
    end
    log(string.format("%s: level-up %d given (XP %s -> %s, level %s, BG3 level %d waiting)", name, ups + 1,
        tostring(before.Total), tostring(after.Total), tostring(after.Level), level))
    return {Name = name, Uuid = u, LevelUps = ups + 1, ToLevel = level, XpBefore = before.Total, Xp = after.Total,
        Level = after.Level, Available = after.Available, Given = math.max(0, need), Leaked = leaked,
        PoolLeft = M.Available()}
end

-- party levelup NAME N: N level-ups at once, so BG3's Level Up button comes up N times in a row and
-- the player clicks through BG3's own screens without leaving the character sheet. Nothing is granted
-- unless the pool holds all N and the member has room for all N (the same cap and XP table as for one).
function M.LevelUpMany(u, n, wait)
    M.RefreshWowLevel()
    n = math.tointeger(n) or 1
    need(n >= 1, "how many level-ups: 1 or more")
    if n == 1 then return M.LevelUp(u, wait) end
    local name = nameOf(u)
    local have = M.Available()
    need(have >= n, string.format("%d level-up(s) asked, the pool has %d (WoW level %d earned %d, %d given)",
        n, have, M.WowLevel(), M.Earned(), M.Spent()))
    local from = 1 + upsOf(u)
    need(from + n <= grantCap(), string.format(
        "%s is at BG3 level %d: %d level-up(s) would pass level %d, the highest the WoW levels give; room for %d",
        name, from, n, grantCap(), math.max(0, grantCap() - from)))
    need(from + n <= M.MaxXpLevel(), string.format(
        "BG3 level %d needs a level-cap mod (BG3's XP table ends at %d); room for %d", from + n, M.MaxXpLevel(),
        math.max(0, M.MaxXpLevel() - from)))
    local first, last, leaked, steps = nil, nil, {}, {}
    for _ = 1, n do
        local r = M.LevelUp(u, wait)
        first, last = first or r, r
        steps[#steps + 1] = string.format("level-up %d: BG3 level %d (XP %s)", r.LevelUps, r.ToLevel, tostring(r.Xp))
        for _, l in ipairs(r.Leaked or {}) do leaked[#leaked + 1] = l end
    end
    log(string.format("%s: %d level-ups given in one go (BG3 level %d to %d)", name, n, from, last.ToLevel))
    return {Name = name, Uuid = u, Count = n, LevelUps = last.LevelUps, ToLevel = last.ToLevel,
        XpBefore = first.XpBefore, Xp = last.Xp, Level = last.Level, Available = last.Available,
        Given = (last.Xp or 0) - (first.XpBefore or 0), Leaked = leaked, Steps = steps, PoolLeft = M.Available()}
end

-- party catchup: every level-up left handed out evenly across the party, one at a time to whoever has
-- had the fewest (the party's order breaks ties), skipping anyone at the cap or at the end of BG3's XP
-- table. Each one is an ordinary grant (M.LevelUp), so every class choice is still the player's.
function M.Catchup(party, wait)
    M.RefreshWowLevel()
    need(type(party) == "table" and #party > 0, "no party member to give level-ups to")
    need(M.Available() > 0, string.format("no level-up to give: WoW level %d earned %d (one per WoW level from %d), %d given",
        M.WowLevel(), M.Earned(), M.FIRST_EARN, M.Spent()))
    local got, stopped, total = {}, nil, 0
    for _, u in ipairs(party) do got[u] = 0 end
    while M.Available() > 0 do
        local pick, fewest = nil, nil
        for _, u in ipairs(party) do
            local ups = upsOf(u)
            if capRoom(ups) and 2 + ups <= M.MaxXpLevel() and (not pick or ups < fewest) then pick, fewest = u, ups end
        end
        if not pick then
            stopped = string.format("nobody has room for another BG3 level (BG3's XP table ends at %d), %d level-up(s) stay in the pool",
                M.MaxXpLevel(), M.Available())
            break
        end
        local ok, err = pcall(M.LevelUp, pick, wait)
        if not ok then stopped = string.format("%s: %s", nameOf(pick), tostring(err)); break end
        got[pick] = got[pick] + 1
    end
    local hero, out = Hero.Uuid(), {}
    for _, u in ipairs(party) do
        local x = xpOf(u) or {}
        total = total + got[u]
        out[#out + 1] = {Uuid = u, Name = nameOf(u), Hero = u == hero, Got = got[u], LevelUps = pool.Ups[u],
            Level = x.Level, ToLevel = 1 + (pool.Ups[u] or 0), Xp = x.Total}
    end
    log(string.format("catchup: %d level-up(s) handed out across %d member(s), %d left in the pool", total, #party,
        M.Available()))
    return {Given = total, Members = out, Stopped = stopped, PoolLeft = M.Available()}
end

-- party.sh reset NAME: Withers' respec; its level-ups go back to the pool when BG3 reports it done.
local respec = {}
function M.Reset(u)
    local name = nameOf(u)
    need(q(Osi.IsInCombat, u) ~= 1, "not during a fight")
    need(not respec[u], name .. " is already in BG3's respec screen")
    upsOf(u)
    respec[u] = {Ups = pool.Ups[u]}
    local ok, err = pcall(Osi.StartRespec, u)
    if not ok then respec[u] = nil; error("StartRespec failed: " .. tostring(err), 0) end
    log(name .. ": respec started (Withers' StartRespec)")
    return {Name = name, Uuid = u, LevelUps = pool.Ups[u], Respec = "started: finish BG3's respec screen; its level-ups go back to the pool then"}
end
local function respecDone(c)
    local u = uuidOf(c)
    -- With companions at camp the player respecs at Withers' own dialog, which no M.Reset announced.
    if not u or (not respec[u] and not camp) then return end
    local back = pool.Ups[u] or 0
    respec[u] = nil
    offered[u], granted[u] = nil, nil
    pcall(setXp, u, 0)
    pool.Ups[u] = 0
    savePool()
    log(string.format("%s: respec done, BG3 level 1, XP 0, %d level-up(s) back in the pool", nameOf(u), back))
end
local function respecCancelled(c)
    local u = uuidOf(c)
    if u and respec[u] then respec[u] = nil; log(nameOf(u) .. ": respec cancelled, nothing changed") end
end
function M.InRespec(u) return respec[u] ~= nil end

-- The popups (companions at camp; BG3's own message box): reaching WoW level 2 and 5 once each, the first
-- member at BG3's own cap with level-ups waiting (no level mod) once, and every refused "join me". They
-- wait in the pool, so a save keeps them, and only open out of every fight and
-- dialog, with no Level Up or respec screen up, one at a time.
-- Keep each under 210 characters: Accept on a box of 340 crashed BG3 every time (a worker thread, right after
-- MessageBoxClosed), 209 did not.
local POPUPS = {
    recruit = "You reached level 2! You can recruit a new companion. Ask them in camp to join your party. "
        .. "Levels 3 and 4 unlock an extra slot.",
    refused = "You're not high enough level for another companion. Levels 2, 3 and 4 each unlock a slot.",
    levels = string.format("You reached level 5! From now on, each level levels up one party member, and every %dth levels up two. "
        .. "Use the Level Up button. At level %d, a party of four is BG3 level %d.",
        M.CURVE.Every, M.WOW_MAX, M.CURVE.Cap),
    cap = "A party member reached level 12, BG3's limit, and has level-ups waiting. To go higher, add a level mod: "
        .. "UnlockLevelCurve or Expansion, with 5e Spells and Advanced Tabletop Spells.",
}
M.POPUPS = POPUPS
function M.Popup(id)
    if not camp or not POPUPS[id] then return end
    for _, q1 in ipairs(pool.Queue) do if q1 == id then return end end
    pool.Queue[#pool.Queue + 1] = id
    savePool()
    log("popup waits: " .. id)
end
local function ups(n) return string.format("%d level-up%s", n, n == 1 and "" or "s") end
-- What the next WoW level gives, for the notice below; nil at the top.
local function nextLine()
    local l = M.WowLevel()
    if l >= M.WOW_MAX then return nil end
    local at = math.max(l + 1, M.FIRST_EARN)
    return string.format("Next: WoW level %d, %s.", at, ups(isDouble(at) and 2 or 1))
end
local dialogs, popupOpen, popupAt, calm = {}, nil, 0, 0
local CALM = 3   -- checks in a row (30 ticks each) with nothing in the way before one opens
local STUCK = 900 -- checks after which an open box without BG3's MessageBoxClosed counts as closed: the next opens over it
local AFTER_LOAD = 45 -- checks after a load before one opens: Running comes before the loading screen goes
local sinceLoad = 0
local function popupDone(id)
    for i, q1 in ipairs(pool.Queue) do
        if q1 == id then table.remove(pool.Queue, i); break end
    end
    popupOpen = nil
    savePool()
end
function M.PopupTick(party)
    if not camp then return end
    local lvl = M.WowLevel()
    for _, t in ipairs({{2, "recruit"}, {5, "levels"}}) do
        if lvl >= t[1] and not pool.Seen[t[2]] then
            pool.Seen[t[2]] = true
            M.Popup(t[2])
            savePool()
        end
    end
    if not pool.Seen.cap and M.MaxXpLevel() <= 12 and M.Available() > 0 then
        for _, u in ipairs(party) do
            local ups = pool.Ups[u]
            if math.type(ups) == "integer" and 1 + ups >= 12 then
                pool.Seen.cap = true
                M.Popup("cap")
                break
            end
        end
    end
    if popupOpen then
        popupAt = popupAt + 1
        if popupAt > STUCK then
            log("popup " .. popupOpen .. ": no MessageBoxClosed, counted as closed")
            popupDone(popupOpen)
        end
        return
    end
    local blocked = next(dialogs) ~= nil or levelUpFor(party) ~= nil
    for _, u in ipairs(party) do
        if q(Osi.IsInCombat, u) == 1 or M.InRespec(u) then blocked = true end
    end
    calm = blocked and 0 or calm + 1
    sinceLoad = sinceLoad + 1
    if sinceLoad < AFTER_LOAD then return end
    local id = pool.Queue[1]
    if calm < CALM then return end
    local host = q(Osi.GetHostCharacter)
    if not id then
        -- A new WoW level, once nothing is in the way: BG3's small notice, no box to close.
        if M.ding and host then
            M.ding = false
            local text = string.format("WoW level %d of %d: %s waiting. %s", M.WowLevel(), M.WOW_MAX,
                ups(math.max(0, M.Available())), nextLine() or "")
            log("notice: " .. text)
            pcall(Osi.ShowNotification, host, text)
        end
        return
    end
    if not host or not POPUPS[id] then
        if not POPUPS[id] then popupDone(id) end
        return
    end
    local ok, err = pcall(Osi.OpenMessageBox, host, POPUPS[id])
    if not ok then log("popup " .. id .. " failed: " .. tostring(err)); return end
    popupOpen, popupAt, calm = id, 0, 0
    log("popup shows: " .. id)
end
local function popupClosed(_, text)
    if popupOpen and text == POPUPS[popupOpen] then
        log("popup closed: " .. popupOpen)
        popupDone(popupOpen)
    end
end
-- One thing per save (Party.lua: a companion placed at camp once).
function M.Once(key)
    if not camp or pool.Seen[key] then return false end
    pool.Seen[key] = true
    savePool()
    return true
end
-- BG3's own party limit follows the slots: its GEN_MaxPlayerCountReached flag is what BG3's "join me"
-- dialogs and PROC_GLO_PartyMembers_CheckAdd read. Never below the party it already has.
local appliedSlots, recheck = nil, false
local function applySlots(party)
    if not camp then return end
    local want = math.max(M.Slots(), #party)
    if appliedSlots == want then
        -- The new limit only counts after this frame: the same call's check reads the old one, and left the
        -- flag one step behind (a join through at WoW 1, BG3's "party full" at WoW 2 with nobody in it).
        if recheck then
            recheck = false
            pcall(Osi.PROC_CheckPartyFull)
        end
        return
    end
    Osi.SetMaxPartySizeOverride(want)
    appliedSlots, recheck = want, true
    log(string.format("BG3's party limit is %d (WoW level %d)", want, M.WowLevel()))
end
-- A "join me" refused: BG3's recruit proc or its party-full swap dialog, while the WoW level is the limit.
local SWAP_DIALOGS = {} -- filled from BG3's DB_GLO_CompanionSwapDialogue at session start
-- A companion's own dialog, one not in the party: at the limit BG3 answers "join me" there with a line ("so many
-- followers") and no add at all, so it counts as tried when it opens. Filled from DB_OriginInPartyDialog.
local IN_PARTY = {}
-- BG3's flag does not stop the dialog's own join when the party is at the limit: whoever came in over it goes
-- back to camp once the dialog is over.
local overJoin = {}
local function joinTried(party, who)
    if camp and M.Slots() < M.SLOT_LEVELS and #party >= M.Slots() then
        M.Popup("refused")
        if who then overJoin[uuidOf(who)] = true end
    end
end
local function sendBack(party)
    if not next(overJoin) or next(dialogs) then return end
    for u in pairs(overJoin) do
        overJoin[u] = nil
        for _, m in ipairs(party) do
            if m == u and #party > M.Slots() then
                pcall(Osi.PROC_GLO_PartyMembers_Remove, u, q(Osi.GetHostCharacter), 1, 0)
                pcall(Osi.PROC_DismissToCamp, u)
                pcall(Osi.SetHasDialog, u, 1)
                appliedSlots = nil
                log("over the limit, back to camp: " .. tostring(u))
            end
        end
    end
end

-- Harness research hook: wow-bg3/progression-research.lua (a chunk returning a function of M) runs
-- when wow-bg3/progression-research-cmd.json {RunId, Sequence} changes; result in
-- wow-bg3/progression-research-result.json.
local function research(runId, st)
    if not Harness.On then return end
    local raw = Ext.IO.LoadFile("wow-bg3/progression-research-cmd.json")
    local ok, c = pcall(Ext.Json.Parse, raw or "")
    if not ok or type(c) ~= "table" or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= st.seq then return end
    st.seq = c.Sequence
    local okr, value = pcall(function()
        local src = assert(Ext.IO.LoadFile("wow-bg3/progression-research.lua"), "no progression-research.lua")
        return assert(load(src))()(M)
    end)
    Ext.IO.SaveFile("wow-bg3/progression-research-result.json", Ext.Json.Stringify({RunId = runId, Sequence = c.Sequence,
        OK = okr, Value = okr and value or tostring(value)}))
end

function M.Start(runId)
    local tick, st, session = 0, {seq = 0}, false
    -- A command already in the file belongs to the Lua state before this load (bg3le rebuilds them).
    do
        local okc, c = pcall(Ext.Json.Parse, Ext.IO.LoadFile("wow-bg3/progression-research-cmd.json") or "")
        if okc and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then st.seq = c.Sequence end
    end
    pcall(Ext.Osiris.RegisterListener, "RespecCompleted", 1, "after", function(c) pcall(respecDone, c) end)
    pcall(Ext.Osiris.RegisterListener, "RespecCancelled", 1, "after", function(c) pcall(respecCancelled, c) end)
    pcall(Ext.Osiris.RegisterListener, "LeveledUp", 1, "after", function(c) pcall(leveledUp, c) end)
    pcall(Ext.Osiris.RegisterListener, "MessageBoxClosed", 2, "after", function(c, t) pcall(popupClosed, c, t) end)
    pcall(Ext.Osiris.RegisterListener, "DialogStarted", 2, "after", function(d, id)
        dialogs[id] = true
        local who = IN_PARTY[uuidOf(d)]
        if SWAP_DIALOGS[uuidOf(d)] or (who and not q(function() return Osi.DB_Players:Get(who)[1] end)) then
            pcall(joinTried, players())
        end
    end)
    pcall(Ext.Osiris.RegisterListener, "DialogEnded", 2, "after", function(_, id) dialogs[id] = nil end)
    pcall(Ext.Osiris.RegisterListener, "PROC_GLO_PartyMembers_CheckAdd", 2, "before", function(who) pcall(joinTried, players(), who) end)
    Ext.Events.GameStateChanged:Subscribe(function(e)
        local to = tostring(e.ToState)
        if to ~= "Running" and to ~= "Save" and to ~= "Paused" and to ~= "GameMasterPause" then session = false end
    end)
    Ext.Events.Tick:Subscribe(function()
        tick = tick + 1
        if Ext.Utils.GetGameState() ~= "Running" then return end
        if not session then
            -- A session began (a load or a new game): the save's pool, BG3's own XP off.
            session = true
            readSwitch()
            dialogs, popupOpen, calm, appliedSlots, sinceLoad, overJoin = {}, nil, 0, nil, 0, {}
            M.ding = false
            for _, r in ipairs(q(function() return Osi.DB_GLO_CompanionSwapDialogue:Get(nil) end) or {}) do SWAP_DIALOGS[uuidOf(r[1])] = true end
            for _, r in ipairs(q(function() return Osi.DB_OriginInPartyDialog:Get(nil, nil) end) or {}) do IN_PARTY[uuidOf(r[2])] = r[1] end
            local had = loadPool()
            for _, u in ipairs(players()) do pcall(upsOf, u) end -- every player counted (levels it has)
            local v = readLevelFile()
            bootSeq, lastSeq = v and v.Seq, nil
            local okx, errx = pcall(M.XpOff)
            if not okx then log("XP off failed: " .. tostring(errx)) end
            log(string.format("session: pool %s (WoW level %s, %d level-up(s) given)", had and "from the save" or "new",
                tostring(pool.WowLevel), M.Spent()))
        end
        if tick % 30 == 7 then pcall(M.RefreshWowLevel) end
        if tick % 30 == 17 then pcall(M.ShowWowProgress) end
        if tick % 90 == 47 then pcall(guard, players()) end
        if tick % 30 == 22 then
            local party = players()
            pcall(sendBack, party)
            pcall(applySlots, party)
            local okP, errP = pcall(M.PopupTick, party)
            if not okP and tick % 900 == 22 then log("popups: " .. tostring(errP)) end
        end
        if tick % 30 == 27 then
            local okO, errO = pcall(M.Offer, players())
            if not okO and tick % 900 == 27 then log("offer: " .. tostring(errO)) end
        end
        if tick % 10 == 5 then pcall(research, runId, st) end
    end)
end

return M
