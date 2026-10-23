-- BG3's menu entries on WoW's mobs and NPCs (server context, started by
-- ActionMonitor.lua). party-multiplayer-design.md "Interaction": a right-click on a WoW mob or NPC
-- opens BG3's context menu (RightAttack.lua gives BG3 the click). What BG3 offers there (harness
-- tests): on a mob's stand-in Attack and Examine (Pickpocket and Throw greyed); on a friendly
-- NPC's stand-in the same, and Talk once the stand-in has a dialog flag (Osiris SetHasDialog: set
-- here on a friendly NPC's stand-in whose WoW unit has something to say, a service in its UNIT_NPC_FLAGS in
-- benilla's link/wowbody/<guid>.txt: gossip, quests, a vendor, a trainer...; cleared on one with
-- nothing, so its menu has no Talk; a party member has Talk and Ungroup of its own). BG3 has no
-- Trade entry for an NPC, its own included (SetCanTrade adds none): a WoW vendor talks, then trades
-- (below).
--   Talk: BG3 walks the hero to the stand-in (its own move-and-talk) and raises
--         DialogStartRequested; BG3 has no dialog for it, so nothing opens in BG3, and WoW's own
--         interact runs on the creature instead: gossip, quests, a vendor's wares (as a left-click).
--   Trade: a WoW vendor trades in BG3's own trade screen, filled by the inventory bridge with the
--         vendor's WoW wares (WOWBG3Inventory.OpenVendor). WoW decides who is a vendor: when WoW
--         opens its merchant window on a creature (after a left-click, or Talk here, and the gossip's
--         "browse your goods"), benilla writes link/wowtrade.bin (WoW -> BG3, 20 bytes: "WTR1", seq,
--         guid lo, hi, seq again; benilla) and closes WoW's window; this module makes that
--         creature's stand-in tradeable (Osiris SetCanTrade), opens BG3's trade with it the way BG3's
--         story does (PROC_StartTrade) and asks inventory for the wares.
--   Examine, Attack...: BG3's own (Examine: the WoW name and level, Names.lua, Portraits.lua).
-- Left clicks on a friendly NPC (WowCursor.lua decides, WorldMode.lua sends; client): the client
-- writes link/friendlyclick.txt, "<seq> talk|pickpocket <guid hex> <stand-in uuid>", and this module
-- runs it: talk as BG3's Talk does (below), pickpocket as BG3's own (Osiris RequestPickpocket, which
-- opens BG3's pickpocket window on the stand-in) once the hero stands within PICK_REACH m of it: one
-- further away (up to PICK_FAR m) is walked to first (Osiris CharacterMoveToPosition, as errands are),
-- as BG3's own click on the stand-in would.
-- The request goes to benilla through link/wowuse.bin (BG3 -> WoW, little endian, 24 bytes):
--   "WUS1", seq u32, the creature's WoW guid lo u32, hi u32, verb u32 (1 talk; 2 reserved), seq again.
-- benilla runs its own dispatcher on that guid as a right-click on it would.
-- Harness: wow-bg3/interact-command.json {RunId, Sequence, Action = "Research"} runs
-- wow-bg3/interact-research.lua (a chunk returning a function of M) into interact-result.json.
-- Report: wow-bg3/link/interact-status.json.
local M = {}
local Hero = Ext.Require("Hero.lua")
local Harness = Ext.Require("Harness.lua")
M.Stats = {Talk = 0, Trade = 0, Ignored = 0, Talkable = 0, Errors = 0}
M.Events = {}
local USE_TALK = 1

local function readJson(path)
    local raw = Ext.IO.LoadFile(path)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

-- Stand-ins (link/mirror-status.json): uuid -> {Guid, Friendly, Dead}; read fresh at most every 10 ticks.
local ticks, standIns, standInsAt = 0, {}, -100
local function standIn(uuid)
    if ticks - standInsAt >= 10 then
        standInsAt = ticks
        local s = readJson("wow-bg3/link/mirror-status.json")
        local set = {}
        if s and type(s.Mobs) == "table" then
            for guid, m in pairs(s.Mobs) do
                if type(m) == "table" and type(m.Uuid) == "string" then
                    set[m.Uuid] = {Guid = guid, Friendly = m.Friendly == true, Dead = m.Dead == true}
                end
            end
        end
        standIns = set
    end
    -- Osiris passes GUIDSTRINGs as "Name_uuid": the uuid is the last 36 characters.
    if type(uuid) ~= "string" or #uuid < 36 then return nil end
    return standIns[uuid:sub(-36)], uuid:sub(-36)
end
M.StandIn = standIn

local useSeq = nil
function M.Use(verb, guid, why)
    assert(type(guid) == "string" and guid:match("^%x+$") and #guid <= 16, "bad guid " .. tostring(guid))
    local g = string.rep("0", 16 - #guid) .. guid
    local hi, lo = tonumber(g:sub(1, 8), 16), tonumber(g:sub(9, 16), 16)
    if not useSeq then
        local old = Ext.IO.LoadFile("wow-bg3/link/wowuse.bin")
        useSeq = (old and #old >= 24 and old:sub(1, 4) == "WUS1") and string.unpack("<I4", old, 5) or 0
    end
    useSeq = useSeq + 1
    Ext.IO.SaveFile("wow-bg3/link/wowuse.bin", string.pack("<c4I4I4I4I4I4", "WUS1", useSeq, lo, hi, verb, useSeq))
    local e = {Seq = useSeq, Verb = verb == USE_TALK and "talk" or tostring(verb), Guid = guid, Why = why, Tick = ticks}
    M.Events[#M.Events + 1] = e
    if #M.Events > 32 then table.remove(M.Events, 1) end
    return e
end

local function note(name, args, what)
    M.Events[#M.Events + 1] = {Event = name, Args = args, What = what, Tick = ticks}
    if #M.Events > 32 then table.remove(M.Events, 1) end
end

-- BG3 asked to talk to a stand-in: WoW talks instead.
local function onDialog(target, player)
    local s = standIn(target)
    if not s then M.Stats.Ignored = M.Stats.Ignored + 1; return end
    if s.Dead then note("DialogStartRequested", {target, player}, "dead"); return end
    M.Stats.Talk = M.Stats.Talk + 1
    M.Use(USE_TALK, s.Guid, "BG3 Talk")
end
-- WoW opened its merchant window on a creature (link/wowtrade.bin): BG3's trade screen instead.
local tradeSeq, opened = nil, {}
function M.OpenTrade(guid, why)
    local uuid
    standIn("")
    for u, s in pairs(standIns) do if s.Guid == guid and not s.Dead then uuid = u end end
    if not uuid then note("OpenTrade", {guid}, "no stand-in"); return nil end
    local hero = Hero.Uuid()
    assert(hero, "no hero")
    pcall(Osi.SetCanTrade, uuid, 1)
    local okT, errT = pcall(Osi.PROC_StartTrade, hero, uuid)
    -- Inventory fills the stand-in with the WoW wares (its own RequestTrade listener covers trades
    -- BG3 opens itself; this one is ours).
    local inv = WOWBG3Inventory -- Inventory.lua's global (server)
    local okV, errV = false, "inventory not loaded"
    if inv and inv.OpenVendor and (opened[uuid] or -1000) < ticks - 60 then
        opened[uuid] = ticks
        okV, errV = pcall(inv.OpenVendor, uuid, guid, hero)
    end
    M.Stats.Trade = M.Stats.Trade + 1
    local e = {Event = "OpenTrade", Guid = guid, Uuid = uuid, Why = why, Trade = okT or tostring(errT),
        Wares = okV or tostring(errV), Tick = ticks}
    M.Events[#M.Events + 1] = e
    if #M.Events > 32 then table.remove(M.Events, 1) end
    return e
end
local function readTrade()
    local raw = Ext.IO.LoadFile("wow-bg3/link/wowtrade.bin")
    local ok = raw and #raw >= 20 and raw:sub(1, 4) == "WTR1"
    local seq, lo, hi, seq2
    if ok then seq, lo, hi, seq2 = string.unpack("<I4I4I4I4", raw, 5) end
    -- The record there at the first look is from before this session.
    if tradeSeq == nil then tradeSeq = (ok and seq == seq2) and seq or -1; return end
    if not ok or seq ~= seq2 then return end
    if seq == tradeSeq then return end
    tradeSeq = seq
    M.OpenTrade(string.format("%08x%08x", hi, lo), "WoW merchant window")
end

-- WoW's UNIT_NPC_FLAGS of a mirrored unit (benilla's wowbody/<guid>.txt), nil until written.
local function npcFlags(guid)
    local text = Ext.IO.LoadFile("wow-bg3/link/wowbody/" .. tostring(guid):lower() .. ".txt")
    return text and tonumber(text:match("npcflags (%d+)")) or nil
end
M.NpcFlags = npcFlags
-- Friendly NPCs' stand-ins get BG3's dialog flag (their menu shows Talk) when WoW has something to say
-- for them, and lose it when not; once per stand-in, once its WoW flags are known.
-- A quest giver with nothing else and nothing for us (WowCursor.lua's link/questsilent.txt, WoW's dialog
-- status 0 or 1 seen on hover) loses Talk as well, and gets it back once the status changes.
local talkable = {}
local questRaw, questSilent = nil, {}
local function readQuestSilent()
    local raw = Ext.IO.LoadFile("wow-bg3/link/questsilent.txt") or ""
    if raw == questRaw then return false end
    questRaw = raw
    local was = questSilent
    questSilent = {}
    for g, st in raw:gmatch("(%x+) (%d+)") do questSilent[g:lower()] = (st == "0" or st == "1") end
    -- Stand-ins whose answer changed are looked at again.
    for uuid, s in pairs(standIns or {}) do
        local g = s.Guid and tostring(s.Guid):lower()
        if g and (was[g] or false) ~= (questSilent[g] or false) then talkable[uuid] = nil end
    end
    return true
end
local function syncTalk()
    standIn("")
    pcall(readQuestSilent)
    for uuid, s in pairs(standIns) do
        if s.Friendly and not s.Dead and talkable[uuid] == nil then
            local flags = npcFlags(s.Guid)
            if flags then
                -- 1.12's services, gossip (1) to repair (0x4000); a guard's 0x10000000 is the server's own.
                local services = flags & 0x7fff
                local talks = services ~= 0 and not (services == 2 and questSilent[tostring(s.Guid):lower()])
                local ok, err = pcall(Osi.SetHasDialog, uuid, talks and 1 or 0)
                talkable[uuid] = ok and talks or tostring(err)
                if ok and talks then M.Stats.Talkable = M.Stats.Talkable + 1 end
                if ok and not talks then M.Stats.Silent = (M.Stats.Silent or 0) + 1 end
            end
        end
    end
end
-- A pickpocket waiting for the hero to reach its NPC (see the header).
local PICK_REACH, PICK_FAR, PICK_WAIT = 2.2, 12.0, 300
local pendingPick = nil -- {Uuid, Until, Moved}
local function posOf(uuid)
    local ok, x, y, z = pcall(Osi.GetPosition, uuid)
    return ok and x and {x, y, z} or nil
end
local function stepPick()
    local p = pendingPick
    if not p then return end
    local hero = Hero.Uuid()
    local hp, tp = posOf(hero), posOf(p.Uuid)
    if ticks > p.Until or not (hp and tp) then
        pendingPick = nil
        note("click", {"pickpocket", p.Uuid}, "not reached")
        return
    end
    local dx, dz = hp[1] - tp[1], hp[3] - tp[3]
    local d = math.sqrt(dx * dx + dz * dz)
    if d <= PICK_REACH then
        pendingPick = nil
        local ok, err = pcall(Osi.RequestPickpocket, hero, p.Uuid)
        M.Stats.Pickpockets = (M.Stats.Pickpockets or 0) + 1
        note("click", {"pickpocket", p.Uuid}, ok and "BG3's pickpocket" or tostring(err))
    elseif d > PICK_FAR then
        pendingPick = nil
        note("click", {"pickpocket", p.Uuid}, "too far")
    elseif not p.Moved then
        p.Moved = true
        local k = (PICK_REACH - 0.6) / d
        pcall(Osi.CharacterMoveToPosition, hero, tp[1] + dx * k, tp[2], tp[3] + dz * k, "Walk", "", 1)
    end
end
-- The client's left clicks on friendly NPCs (see the header); the record there at the first look is
-- from before this session.
local clickSeq = nil
local function readClick()
    local line = Ext.IO.LoadFile("wow-bg3/link/friendlyclick.txt") or ""
    local seq, verb, guid, uuid = line:match("^(%d+) (%a+) (%x+) (%S+)")
    seq = tonumber(seq)
    if clickSeq == nil then clickSeq = seq or -1; return end
    if not seq or seq == clickSeq then return end
    clickSeq = seq
    local s = standIn(uuid)
    if not s or s.Dead then note("click", {verb, guid}, "no living stand-in"); return end
    if verb == "talk" then
        M.Stats.Clicks = (M.Stats.Clicks or 0) + 1
        M.Use(USE_TALK, s.Guid, "left click")
    elseif verb == "pickpocket" then
        pendingPick = {Uuid = uuid:sub(-36), Until = ticks + PICK_WAIT}
        stepPick()
    end
end
function M.Start(runId)
    M.RunId = runId
    local listeners = {}
    local function listen(name, arity, fn)
        local ok, err = pcall(Ext.Osiris.RegisterListener, name, arity, "before", function(...)
            local okf, errf = pcall(fn, ...)
            if not okf then M.Stats.Errors = M.Stats.Errors + 1; note(name, {...}, tostring(errf)) end
        end)
        listeners[name] = ok and arity or tostring(err)
    end
    listen("DialogStartRequested", 2, onDialog)
    M.Listeners = listeners
    local sequence = 0
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 5 == 2 then pcall(readTrade) end
        pcall(readClick)
        if pendingPick and ticks % 3 == 0 then pcall(stepPick) end
        if ticks % 15 ~= 7 then return end
        pcall(syncTalk)
        local c = readJson("wow-bg3/interact-command.json")
        if c and c.RunId == runId and math.type(c.Sequence) == "integer" and c.Sequence > sequence then
            sequence = c.Sequence
            local ok, value = pcall(function()
                assert(c.Action == "Research", "unknown action")
                assert(Harness.On, "harness actions are off (WOW_BG3_HARNESS=1)")
                local src = assert(Ext.IO.LoadFile("wow-bg3/interact-research.lua"), "no interact-research.lua")
                local fn = assert(load(src))()
                return fn(M)
            end)
            Ext.IO.SaveFile("wow-bg3/interact-result.json", Ext.Json.Stringify({RunId = runId, Sequence = sequence,
                OK = ok, Value = ok and value or tostring(value)}))
        end
        if ticks % 60 == 7 then
            local t = {}
            for u, v in pairs(talkable) do t[u] = v end
            Ext.IO.SaveFile("wow-bg3/link/interact-status.json", Ext.Json.Stringify({RunId = runId, Tick = ticks,
                Listeners = listeners, Stats = M.Stats, Events = M.Events, Talkable = t}))
        end
    end)
end
return M
