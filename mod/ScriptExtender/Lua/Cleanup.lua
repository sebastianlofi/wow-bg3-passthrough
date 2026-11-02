-- Cleanup (server): no stand-in outlives its session.
--
-- Stand-ins (Mirrors.lua, StandIns.lua) are Osiris CreateAt characters made "temporary". A save made
-- while they exist (F5, an autosave, the menu) carries them, and nothing removes them after a load:
-- the module tables that owned them (Mirrors.lua's WOWBG3MirrorOwned) are new and empty in the new
-- Lua session, and their WoW names are gone (Names.lua's SetStoryDisplayName gives a runtime
-- "ResStr_..." handle that a load does not bring back: "Not Found" or empty). Harness:
-- the test save (Tav/QuickSave_2) holds two, a rat stand-in (WoW's Rabbit) and a human one (Deputy
-- Willem), level 1, 15 and 21 m from the void centre; every harness start loads that save, so they
-- were back every time. They keep their Temporary flag: RequestDeleteTemporary removes them at once.
--
-- Mark: every stand-in alive in this session carries a persistent user variable (Ext.Vars
-- "WowBg3StandIn" = this session's token), so a save carries the mark with it.
-- Sweep: after a load, once the game runs (and twice more as the level streams in), every server
-- character that carries a mark from another session, or is a legacy one (from before the mark: a
-- stand-in template of the bridge's list whose display name is a runtime "ResStr_" handle, see
-- M.Legacy; first sweep only), is removed: RequestDeleteTemporary, and for one still there 15 ticks
-- later SetOnStage 0 and RequestDelete. Players, party members and anything owned in this session
-- are never touched.
-- Status: wow-bg3/cleanup.json {RunId, Token, Marked, Swept = [{Uuid, Name, Template, Pos, Why, How,
-- Gone}], Stale}.
--
-- The mid-fight save rule: a save made during a BG3 fight loads straight back into that fight. BG3
-- keeps its own combat in the save, the fight bridge does not, so after such a load the party stands
-- in a turn-based fight whose enemies are the stand-ins of the session that saved (swept above) and
-- no WoW fight runs behind it: BG3 waits for turns from a bridge that has never heard of this fight,
-- and nothing ends it.
-- Rule: in a world-mode session, in the first STALE_TO ticks after the level runs, a party combat
-- (Party.Combat) while the bridge runs no fight of this session (LinkLive.FightLive) is BG3's
-- leftover, and BG3's own EndCombat ends it where everyone stands: no teleport, no damage, nobody
-- removed. A bridged fight is left alone and the rule gives up for the rest of the session, which no
-- later look can undo. A character this session made proves nothing (seen in a live test): world
-- mode is up before the first Running tick (the held loading screen), and BG3 pulls a freshly
-- mirrored hostile stand-in into the combat the save carried, which the bridge then adopts. It ends at most one combat per session; without world mode
-- (WorldFlag) it never looks, so a vanilla BG3 game keeps its own saved fights. What it did goes in
-- cleanup.json (Stale) and the extender log.
local M = {}
-- Under pcall: a module that fails to load turns off the mid-fight save rule only, never the sweep.
local okParty, Party = pcall(Ext.Require, "Party.lua")          -- the party's own BG3 combat
local okFlag, WorldFlag = pcall(Ext.Require, "WorldFlag.lua")  -- world mode, or a vanilla BG3 game

local VAR = "WowBg3StandIn"
pcall(Ext.Vars.RegisterUserVariable, VAR, {Server = true, Client = false, SyncToClient = false, Persistent = true})

-- The stand-in root templates (bridge/src/bridge.rs STANDINS: wolf, boar, bear, spider, rat, zombie,
-- goblin, human).
M.TEMPLATES = {
    ["41da0eff-b533-43cc-a014-218f7d23fa11"] = "wolf", ["b9186958-8507-4076-ade5-0a3b43763997"] = "boar",
    ["b2754d31-198e-4ae6-87ec-5399a3669639"] = "bear", ["8ed12d53-90aa-4df0-a027-a7b4a022714c"] = "spider",
    ["aa60d129-8d11-4178-a22b-836062e86010"] = "rat", ["a42a8493-b701-4d71-bdc8-0857e718c247"] = "zombie",
    ["2db928f2-f1c5-4b5a-8751-168f9f292249"] = "goblin", ["d3b7cade-c52e-47dc-816d-067c00d08f37"] = "human",
}
local SWEEPS = {60, 300, 900} -- ticks after the first Running tick of the session
-- The mid-fight save rule's window after the first Running tick, and how often it is looked at.
local STALE_FROM, STALE_TO, STALE_EVERY = 20, 900, 10
local STALE_CHECK = 30 -- ticks after EndCombat before the combat is looked at again

local function q(fn, ...)
    local ok, v = pcall(fn, ...)
    if ok then return v end
    return nil
end
local function uuidOf(s) return type(s) == "string" and s:match("(%x+%-%x+%-%x+%-%x+%-%x+)$") or nil end

-- Stand-ins other modules made in this session: Mirrors.lua's table, plus any module's M.Mark.
local extra = {}
function M.Mark(uuid) if uuidOf(uuid) then extra[uuidOf(uuid)] = true end end
local function owned()
    local all = {}
    for u in pairs(WOWBG3MirrorOwned or {}) do all[u] = true end
    for u in pairs(extra) do all[u] = true end
    return all
end

-- A stand-in from before the mark: a stand-in template whose display name is a runtime handle
-- ("ResStr_...": Names.lua's SetStoryDisplayName). BG3's own characters carry localized handles.
function M.Legacy(id, e)
    local tpl = uuidOf(q(Osi.GetTemplate, id) or "")
    if not tpl or not M.TEMPLATES[tpl] then return nil end
    local handle = q(function() return tostring(e.DisplayName.Name.Handle.Handle) end)
    if type(handle) == "string" and handle:find("^ResStr_") then
        return "legacy " .. M.TEMPLATES[tpl] .. " stand-in (" .. handle .. ")"
    end
end

local function protected(id)
    if q(Osi.IsPlayer, id) == 1 or q(Osi.IsPartyMember, id, 1) == 1 then return true end
    for _, row in ipairs(q(function() return Osi.DB_Players:Get(nil) end) or {}) do
        if uuidOf(tostring(type(row) == "table" and row[1] or row)) == id then return true end
    end
    return false
end

function M.Start(runId)
    local token = tostring(runId) .. ":" .. tostring(q(Ext.Utils.MonotonicTime) or os.time())
    local ticks, running, marked = 0, nil, 0
    local swept, pending, stale = {}, {}, {}
    M.State = {Token = token, Swept = swept, Stale = stale}

    local function status()
        Ext.IO.SaveFile("wow-bg3/cleanup.json", Ext.Json.Stringify({RunId = runId, Token = token, Tick = ticks,
            Marked = marked, Swept = swept, Stale = stale}))
    end

    local function remove(id, why)
        local row = {Uuid = id, Why = why, At = ticks,
            Name = q(function() return Osi.ResolveTranslatedString(Osi.GetDisplayName(id)) end),
            Template = q(Osi.GetTemplate, id)}
        local okp, x, y, z = pcall(Osi.GetPosition, id)
        row.Pos = okp and type(x) == "number" and {x, y, z} or nil
        local flags = q(function() return Ext.Entity.Get(id).ServerCharacter.Flags end)
        row.Temporary = type(flags) == "table" and (flags.Temporary == true or (function()
            for _, v in pairs(flags) do if v == "Temporary" then return true end end
            return false
        end)()) or nil
        if row.Temporary ~= false then row.How = pcall(Osi.RequestDeleteTemporary, id) and "RequestDeleteTemporary" or nil end
        pending[id] = row
        swept[#swept + 1] = row
        Ext.Utils.Print("WOW-BG3 cleanup: removing " .. tostring(row.Name) .. " " .. id .. " (" .. why .. ")")
    end

    -- Second step for one that is still there: no longer temporary, so off stage and deleted.
    local function finish()
        for id, row in pairs(pending) do
            if ticks - row.At >= 15 then
                if q(Ext.Entity.Get, id) == nil then
                    row.Gone, pending[id] = true, nil
                elseif not row.Second then
                    row.Second = true
                    pcall(Osi.SetOnStage, id, 0)
                    row.How = (row.How and row.How .. ", " or "") .. (pcall(Osi.RequestDelete, id) and "SetOnStage 0 + RequestDelete" or "RequestDelete refused")
                    row.At = ticks
                else
                    row.Gone, pending[id] = (q(Ext.Entity.Get, id) == nil), nil
                end
            end
        end
    end

    -- legacy: only in the first sweep, before any fight of this session can have made a stand-in
    -- outside Mirrors.lua's table (StandIns.lua's for LinkLive).
    local function sweep(legacy)
        local mine = owned()
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ServerCharacter")) do
            local id = q(function() return tostring(e.Uuid.EntityUuid) end)
            if id and not mine[id] and not pending[id] and not protected(id) then
                local mark = q(function() return e.Vars[VAR] end)
                if type(mark) == "string" and mark ~= token then
                    remove(id, "stand-in of session " .. mark)
                elseif mark == nil and legacy then
                    local why = M.Legacy(id, e)
                    if why then remove(id, why) end
                end
            end
        end
    end

    local function markAll()
        for id in pairs(owned()) do
            local e = q(Ext.Entity.Get, id)
            if e and q(function() return e.Vars[VAR] end) ~= token then
                if pcall(function() e.Vars[VAR] = token end) then marked = marked + 1 end
            end
        end
    end

    -- Who is in BG3 combat `combat`: every server character in it, and the first one this session
    -- made (a stand-in of a fight the bridge knows about).
    local function inCombat(combat)
        local mine, all, ours = owned(), {}, nil
        for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ServerCharacter")) do
            local id = q(function() return tostring(e.Uuid.EntityUuid) end)
            if id and q(Osi.IsInCombat, id) == 1 and tostring(q(Osi.CombatGetGuidFor, id) or "") == combat then
                all[#all + 1] = id
                if not ours and (mine[id] or q(function() return e.Vars[VAR] end) == token) then ours = id end
            end
        end
        return all, ours
    end

    -- The mid-fight save rule (see the header): BG3's own combat, back from the save with no fight
    -- behind it, ended where everyone stands.
    local function leftover()
        if not (okParty and okFlag) then
            stale.Done, stale.Error = true, "Party.lua or WorldFlag.lua did not load"
            return status()
        end
        if WorldFlag.On() ~= true then return end
        local combat = q(Party.Combat)
        if type(combat) ~= "string" or combat == "" then return end
        local all, ours = inCombat(combat)
        local okL, LinkLive = pcall(Ext.Require, "LinkLive.lua")
        if okL and type(LinkLive) == "table" and LinkLive.FightLive then
            -- The bridge's own fight: never touched, and the rule gives up for this session, so no
            -- later look can take a fight of its own for a leftover.
            stale.Done, stale.Mine = true, ours or "bridge"
            Ext.Utils.Print("WOW-BG3 cleanup: combat " .. combat .. " is the bridge's own fight, left alone")
            return status()
        end
        stale.Done, stale.Combat, stale.At, stale.Who, stale.Fresh = true, combat, ticks, all, ours
        Ext.Utils.Print("WOW-BG3 cleanup: BG3's combat " .. combat .. " came back with the save (" ..
            #all .. " in it, no bridged fight in this session" .. (ours and (", " .. ours .. " pulled in since the load") or "") ..
            "): ending it where everyone stands")
        stale.Ended = pcall(Osi.EndCombat, combat)
        stale.Check = ticks + STALE_CHECK
        status()
    end

    -- EndCombat did not take: everyone leaves the combat instead (Osiris LeaveCombat, as Mirrors.lua
    -- uses to pull a stand-in back out of one).
    local function leftoverCheck()
        stale.Check = nil
        stale.StillOn = q(Osi.CombatIsActive, stale.Combat) == 1 or tostring(q(Party.Combat) or "") == stale.Combat
        if not stale.StillOn then
            Ext.Utils.Print("WOW-BG3 cleanup: BG3's leftover combat is over")
            return status()
        end
        stale.Left = 0
        for _, id in ipairs((inCombat(stale.Combat))) do
            if pcall(Osi.LeaveCombat, id) then stale.Left = stale.Left + 1 end
        end
        Ext.Utils.Print("WOW-BG3 cleanup: EndCombat left it running, " .. stale.Left .. " sent out of it")
        status()
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if Ext.Utils.GetGameState() ~= "Running" then return end
        running = running or ticks
        if ticks % 15 == 0 then pcall(markAll) end
        if not stale.Done and ticks % STALE_EVERY == 0
            and ticks - running >= STALE_FROM and ticks - running <= STALE_TO then
            local ok, err = pcall(leftover)
            if not ok then Ext.Utils.Print("WOW-BG3 cleanup: leftover combat: " .. tostring(err)) end
        end
        if stale.Check and ticks >= stale.Check then
            local ok, err = pcall(leftoverCheck)
            if not ok then Ext.Utils.Print("WOW-BG3 cleanup: leftover combat check: " .. tostring(err)) end
        end
        for _, at in ipairs(SWEEPS) do
            if ticks - running == at then
                local ok, err = pcall(sweep, at == SWEEPS[1])
                if not ok then Ext.Utils.Print("WOW-BG3 cleanup: " .. tostring(err)) end
                status()
            end
        end
        if next(pending) and ticks % 5 == 0 then
            pcall(finish)
            if not next(pending) then status() end
        end
    end)
end

return M
