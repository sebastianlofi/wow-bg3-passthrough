-- The hero (the mod follows whichever character the player controls,
-- not Tav's fixed UUID). One source of truth for every module, on both sides: the server's host
-- character, Osiris GetHostCharacter (the character the host user controls; in this single-player
-- setup the player's own, e.g. the custom Tav of the test save or an origin such as Gale).
--   server: read once per tick (cached), written to wow-bg3/hero.json within 15 ticks of a change;
--   client: that file (same process, same folder; read every 15 ticks). The client has no reliable
--           source of its own: a hero standing in the void when a save loads is a stub on the client
--           until world mode rebuilds BG3's spatial grid (no party components, and the HUD's
--           SelectedCharacter is nil; harness, Tav's world-mode quicksave).
-- World mode teleports the hero it had at Start and brings that one back at Stop (WorldMode.lua).
-- hero.json also serves the tools outside BG3 (world bridge, test harness helpers):
-- {RunId, Uuid, Name, Region, Ready, NewGame, ...}; Ready: the hero is in the tutorial level
-- (TUT_Avernus_C, the level world mode's void is made for), the game runs, no dialog holds the hero,
-- and (a new game, NewGame.lua) the level's gameplay has started.
-- Tav, the custom character of the test save (wow-bg3-m4-setup-world), is just one possible hero now.
local M = {}
M.REGION = "TUT_Avernus_C"
-- The host level (a bigger level for more ground): wow-bg3/host.json may name another,
-- {"Region": "WLD_Main_A", "Spot": [x, y, z]} (Spot: where a hero whose home is in the void goes
-- back to at Stop). Every module asks Hero for it instead of naming the tutorial level itself.
M.SPOT = {-46.58, 2.82, -402.83}
pcall(function()
    local raw = Ext.IO.LoadFile("wow-bg3/host.json")
    local h = raw and #raw < 4096 and Ext.Json.Parse(raw)
    if type(h) == "table" and type(h.Region) == "string" and h.Region:find("^[%w_]+$") then M.REGION = h.Region end
    if type(h) == "table" and type(h.Spot) == "table" and #h.Spot == 3 then M.SPOT = h.Spot end
end)
-- The test save's custom character, for tools and logs only (never used to pick the hero).
M.TAV = "24f08375-99c0-8433-10d1-e6c218008716"

local client = Ext.IsClient()
local tick, cached, cachedTick = 0, nil, -1
local runId = nil -- M.Start
-- The cache's clock (this module runs once per side: Ext.Require keeps it).
Ext.Events.Tick:Subscribe(function() tick = tick + 1 end)

local function uuidOf(s)
    return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil
end
M.UuidOf = uuidOf

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end

local fileTick = -1000
local function fromFile()
    local raw = Ext.IO.LoadFile("wow-bg3/hero.json")
    local ok, h = pcall(Ext.Json.Parse, raw or "")
    return ok and type(h) == "table" and h.RunId == runId and uuidOf(h.Uuid) or nil
end

-- A party: the host character is whoever the player selects, a companion too (BG3
-- selects whoever's turn it is in a fight). The WoW character is one BG3 character, so while a
-- companion is selected the hero stays the user's avatar (Tav, or the origin a new game started with);
-- selecting an avatar makes it the hero at once.
local AVATAR = "306b9b05-1057-4770-aa17-01af21acd650"
local lastHero = nil
local function pickHero()
    local host = uuidOf(q(Osi.GetHostCharacter))
    if not host or q(Osi.IsTagged, host, AVATAR) == 1 then
        lastHero = host or lastHero
        return host
    end
    local user = q(Osi.GetReservedUserID, host)
    local found = nil
    local ok, avatars = pcall(function() return Osi.DB_Avatars:Get(nil) end)
    for _, row in ipairs(ok and avatars or {}) do
        local a = uuidOf(tostring(row[1]))
        if a and user ~= nil and q(Osi.GetReservedUserID, a) == user then
            if a == lastHero then return a end
            found = found or a
        end
    end
    if found then lastHero = found; return found end
    return host
end

-- The hero's UUID (nil while there is none: main menu, loading).
function M.Uuid()
    if client then
        if tick - fileTick >= 15 or (cached == nil and cachedTick ~= tick) then
            cached, fileTick, cachedTick = fromFile(), tick, tick
        end
        return cached
    end
    if cachedTick ~= tick or cached == nil then
        cached, cachedTick = pickHero(), tick
    end
    return cached
end
-- Whether a UUID or an Osiris GUIDSTRING ("Name_uuid") names the hero.
function M.Is(s)
    local h = M.Uuid()
    return h ~= nil and type(s) == "string" and s:find(h, 1, true) ~= nil
end
function M.Entity()
    local u = M.Uuid()
    return u and Ext.Entity.Get(u) or nil
end
function M.Position()
    local e = assert(M.Entity(), "no hero")
    local t = e.Transform.Transform.Translate
    return {t[1], t[2], t[3]}
end
-- The guard the harness channels keep (was "the authorized disposable save"): a hero in the level
-- world mode is made for. The client cannot ask Osiris for the region and checks the entity only.
function M.Assert()
    local u = assert(M.Uuid(), "no hero (no character controlled)")
    assert(Ext.Entity.Get(u), "hero entity unavailable")
    -- The host level, or the tutorial level a save starts in before it is sent to the host level.
    if not client then
        local r = Osi.GetRegion(u)
        assert(r == M.REGION or r == "TUT_Avernus_C", "hero not in " .. M.REGION)
    end
    return u
end

-- Race, class and body of any character: BG3 static data names; a sub-race (High Elf)
-- also gives its parent (Elf). Used for the hero (hero.json) and every party member (WowSave.lua's
-- lifecycle.json): link/tools/wowchar.py turns them into a WoW race, class and gender.
function M.Body(u)
    local out = {}
    pcall(function()
        local e = Ext.Entity.Get(u)
        local function sd(id, kind)
            if type(id) ~= "string" or id == "00000000-0000-0000-0000-000000000000" then return nil end
            local ok, d = pcall(Ext.StaticData.Get, id, kind)
            return ok and d or nil
        end
        local race = e and e.Race and sd(e.Race.Race, "Race")
        if race then
            out.Race = tostring(race.Name)
            local parent = sd(race.ParentGuid, "Race")
            out.RaceParent = parent and tostring(parent.Name) or out.Race
        end
        local first = e and e.Classes and e.Classes.Classes and e.Classes.Classes[1]
        local cd = first and sd(first.ClassUUID, "ClassDescription")
        if cd then out.Class = tostring(cd.Name) end
        if e and e.BodyType then out.BodyType = e.BodyType.BodyType end
    end)
    out.Template = q(Osi.GetTemplate, u)
    return out
end

-- This Lua state's session (lifecycle): bg3le rebuilds the Lua states on every load
-- (UnloadSession), so a fresh token here marks a fresh BG3 session (a load, a new game) for the tools
-- outside BG3, even when the run (WOW_BG3_RUN_ID) and the save are the same as before.
local function newSession() return string.format("%x%04x", math.floor(Ext.Utils.MonotonicTime()), math.random(0, 0xffff)) end
M.SESSION = newSession()
-- A load that keeps this Lua state (none seen so far, but bg3le only rebuilds at UnloadSession) is a new
-- session too.
Ext.Events.GameStateChanged:Subscribe(function(e)
    if tostring(e.ToState) == "LoadSession" then M.SESSION = newSession() end
end)

local function heroInfo(runId)
    local u = M.Uuid()
    local info = {RunId = runId, Session = M.SESSION, Uuid = u, Tick = tick, Time = Ext.Utils.MonotonicTime()}
    if not u then return info end
    info.Region = q(Osi.GetRegion, u)
    info.Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(u)) end)
    info.Template = q(Osi.GetTemplate, u)
    info.Avatar = q(Osi.IsTagged, u, "306b9b05-1057-4770-aa17-01af21acd650") == 1
    info.Origin = false
    for _, row in ipairs(q(function() return Osi.DB_Origins:Get(nil) end) or {}) do
        if M.Is(tostring(row[1])) then info.Origin = true end
    end
    local speaking = false
    local rows = q(function() return Osi.DB_DialogSpeakers:Get(nil, nil, nil) end) or {}
    local instance = nil
    for _, row in ipairs(rows) do
        if M.Is(tostring(row[2])) then speaking = true; instance = row[1] end
    end
    info.InDialog = speaking
    -- PartyDialog: the hero's dialog has party members alone as speakers (a companion's talk). BG3
    -- places and turns its actors through the dialog's timeline at render time, which none of what the
    -- layer copies for WoW's figures shows (harness: WoW drew Astarion from behind on his usual spot
    -- while BG3 had him facing Tav, so his face and mouth never showed; a playtest report). For such a
    -- dialog RenderScreen.lua shows BG3's own picture (its 3D screen mode); with a WoW NPC's stand-in
    -- among the speakers the WoW picture stays (its BG3 body must not show).
    if speaking then
        local party = {}
        for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
            local p = uuidOf(tostring(type(row) == "table" and row[1] or row))
            if p then party[p] = true end
        end
        local all, others = true, 0
        for _, row in ipairs(rows) do
            if row[1] == instance then
                local s = uuidOf(tostring(row[2]))
                if not (s and party[s]) then all = false elseif not M.Is(tostring(row[2])) then others = others + 1 end
            end
        end
        info.PartyDialog = all and others > 0
    end
    -- Race, class and body (plug-and-play: the hidden WoW character follows them,
    -- link/tools/wowchar.py maps them): M.Body.
    for k, v in pairs(M.Body(u)) do info[k] = v end
    return info
end

function M.Start(id)
    runId = id
    if client then return end
    local NewGame = Ext.Require("NewGame.lua")
    local last, okSince, every, lastUuid = nil, nil, 30, nil
    Ext.Events.Tick:Subscribe(function()
        -- A change of hero reaches the file (and the client) within 15 ticks; the rest every 30.
        if tick % 15 ~= 0 then return end
        -- A save (the autosave at a long rest's end, a quicksave) lasts under a second and the hero is still there.
        -- A file without it then read as no hero: the inventory bridge listed nobody, BG3's copies of what WoW holds went.
        if Ext.Utils.GetGameState() == "Save" then return end
        local now = Ext.Utils.GetGameState() == "Running" and M.Uuid() or nil
        if tick % every ~= 0 and now == lastUuid then return end
        lastUuid = now
        local running = Ext.Utils.GetGameState() == "Running"
        local info = running and heroInfo(runId) or {RunId = runId, Session = M.SESSION, Tick = tick, State = tostring(Ext.Utils.GetGameState())}
        info.NewGame = NewGame.Enabled()
        local ok = running and info.Uuid ~= nil and info.Region == M.REGION and info.InDialog == false
            and (not info.NewGame or NewGame.LevelStarted == M.REGION)
        if ok then okSince = okSince or tick else okSince = nil end
        -- Ready after 3 s of all of the above (a dialog the intro skip had to stop, a load settling).
        info.Ready = okSince ~= nil and tick - okSince >= 90
        local key = tostring(info.Uuid) .. tostring(info.Region) .. tostring(info.Ready) .. tostring(info.InDialog)
            .. tostring(info.PartyDialog)
        if key ~= last or tick % 300 == 0 then
            -- (written as soon as the hero changes, see above)
            last = key
            Ext.IO.SaveFile("wow-bg3/hero.json", Ext.Json.Stringify(info))
        end
    end)
end

return M
