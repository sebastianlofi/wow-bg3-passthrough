-- World mode: the BG3 window is the game and WoW is the world.
-- Tav lives in the void for the whole session, on WoW ground cut around her (arena files from
-- the WoW side), BG3's camera leads and benilla follows it, and Tav's position goes back to
-- benilla (link/tav.bin) so the WoW character walks where she walks. Party members other than
-- the disposable character go off stage for the session. Nothing here is ever saved: it all
-- lives in memory and is undone by Stop (or by leaving the level).
--
-- Driven by wow-bg3/world-command.json (one command per sequence, client context):
--   Start  {Centre = {x,y,z}, WowOrigin = {x,y,z}, Cells = {{K, MinX, MinZ, BaseY}...}, PoolSize, Layer}
--          ground (the treadmill: pool subgrids placed as 64 m squares of WoW ground, the files
--          wow-bg3/section-<K>.bin and arena-triangles.bin written by the WoW side) + teleport + lead
--   Stream {Cells, Rays}                  Tav walked on: move those pool subgrids, swap the triangles
--   Stop   {}                             Tav back, party back, ground and camera restored
-- The party: the hero's companions come along (Party.lua Gather at Start, with its
-- upkeep; back next to the hero at Stop) and WoW draws them like the hero (TavPlan.lua figures).
-- Music: Start turns BG3's exploration music off (BG3's own Music_Off event
-- through Osiris, StandIns.lua Music), so WoW's zone music and ambience play (benilla 0030); BG3's
-- combat music still plays in fights, and WoW's music yields to it while Tav is in a BG3 combat.
-- Mapping (mirrored, BG3 renders left-handed): BG3 = Centre + (dy, dz, -dx) * 0.9144 for a WoW
-- delta (dx, dy, dz) from WowOrigin.
--
-- Pointer (clicks on WoW's people and things go to WoW): while started, BG3's cursor
-- and counts of left and right clicks go to benilla (link/bg3input.bin: "BGI2", seq, x, y, window
-- w, h, left clicks, right clicks, seq again), which aims WoW's mouseover there and replays a left
-- click, or WoW's interact for a right click (talk, vendor, quest, loot). When WoW
-- says it has its UI or a unit under the pointer (link/wowhover.bin: "WBH1", seq, flags, seq;
-- flags 1 UI, 2 a unit or object WoW takes; 4, a mirrored mob, is BG3's and not tested here),
-- BG3's own action for that click is prevented, so Tav does not also walk there. Friendly NPCs
--: WoW takes a click on a mirrored vendor, quest giver, guard or villager too
-- (benilla 0027), except an attack: when BG3's own cursor over its stand-in is one (Ctrl held,
-- BG3's way to attack someone who is not hostile, or an aimed attack or spell; WowCursor.lua sets
-- WOWBG3BG3TakesClick), the click is BG3's alone and WoW never hears of it.
--
-- Errands (merchants and bodies used from far away): the WoW body never
-- walks by itself, it follows Tav. A click on a WoW NPC, body or object out of reach makes benilla
-- write link/tavgo.bin ("TGO1", seq, guid lo, hi, the target's WoW xyz, stop distance yd, flags:
-- 1 go, 2 stop her; seq again); Tav runs to the stop point short of the target (errand.json for
-- StandIns.lua, server: Osiris CharacterMoveToPosition), out of combat only and only onto ground
-- the treadmill has placed (that call teleports when it finds no path), and benilla runs the verb
-- once the WoW body, following her, is in reach.
local M = {}
local Harness = Ext.Require("Harness.lua")
local Probe = Ext.Require("ActionProbe.lua")
local TavPlan = Ext.Require("TavPlan.lua")
local Session = Ext.Require("Session.lua") -- held game states (a save): keep world mode (F5 fix)
local Hero = Ext.Require("Hero.lua") -- the hero: the character the player controls
local Camp = Ext.Require("Camp.lua") -- the switch `camp` and a trip's numbers (the module runs on the server)
local YD = 0.9144
local MAX_SECONDS = 900

-- Whether world mode is running (LinkLive.lua asks before setting up a fight).
function M.IsActive() return false end

local function readJson(name)
    local raw = Ext.IO.LoadFile("wow-bg3/" .. name)
    if not raw then return nil end
    local ok, v = pcall(Ext.Json.Parse, raw)
    return ok and type(v) == "table" and v or nil
end

function M.Start(runId)
    local ticks, sequence, state, jobs = 0, 0, nil, {}
    -- A load starts this module again with the same RunId: the command already in the file is the
    -- previous session's, done (WorldMode re-ran the old Start after a
    -- load, and the bridge's own Start then failed "the pool is in use"). Taken as done here, at start.
    pcall(function()
        local raw = Ext.IO.LoadFile("wow-bg3/world-command.json")
        local ok, c = pcall(Ext.Json.Parse, raw or "")
        if ok and type(c) == "table" and c.RunId == runId and math.type(c.Sequence) == "integer" then sequence = c.Sequence end
    end)
    local tavSeq, tavFd = 0, nil
    local inSeq, clicks, rclicks, lastX, lastY = 0, 0, 0, nil, nil
    -- BG3's window size for BGI2 (bg3/tools/sandbox.py writes window.json at each launch; 1280x720 for harness runs).
    local win = readJson("window.json") or {}
    local W, H = tonumber(win.Width) or 1280, tonumber(win.Height) or 720

    local function wait(n)
        local due = ticks + n
        while ticks < due do coroutine.yield() end
    end
    local function command(stem, c, results)
        local old = readJson(stem .. ".json")
        local seq = (old and old.RunId == runId and math.type(old.Sequence) == "integer") and old.Sequence + 1 or 1
        c.RunId, c.Sequence = runId, seq
        Ext.IO.SaveFile("wow-bg3/" .. stem .. ".json", Ext.Json.Stringify(c))
        local due, got = ticks + 300, {}
        while true do
            local all = true
            for _, name in ipairs(results) do
                if not got[name] then
                    local r = readJson(name)
                    if r and r.RunId == runId and r.Sequence == seq then got[name] = r else all = false end
                end
            end
            if all then break end
            if ticks > due then error(c.Action .. ": no result", 0) end
            coroutine.yield()
        end
        for _, name in ipairs(results) do
            if got[name].OK ~= true then error(c.Action .. " failed: " .. tostring(got[name].Value), 0) end
        end
        return got[results[1]].Value, got
    end
    local function arena(c) return command("arena-command", c, {"arena-server-result.json", "arena-client-result.json"}) end
    local function standIn(c) return command("stand-in-command", c, {"stand-in-result.json"}) end
    -- The party (Party.lua on the server): companions next to the hero and chained.
    local function party(c) return command("party-command", c, {"party-result.json"}) end
    -- The party WoW draws: the hero first, then the companions (Party.lua's wow-bg3/party.json) within
    -- 60 m of the hero, each a figure of Tav's plan (TavPlan.lua), then their summons (a familiar, a
    -- spirit companion, a raised dead: eoc::summon::IsSummon whose summoner is a drawn figure; its Owner
    -- is the summon itself for Find Familiar's cat (harness); a playtest report: the pet had no model, as
    -- nothing drew it).
    local partyAt, partyList = -1000, {}
    local function figures()
        local hero = Hero.Uuid()
        if not hero then return {} end
        if ticks - partyAt >= 15 then
            partyAt = ticks
            local pj = readJson("party.json")
            partyList = {}
            if pj and pj.RunId == runId and type(pj.Members) == "table" then
                for _, m in ipairs(pj.Members) do
                    if type(m) == "table" and type(m.Uuid) == "string" and m.OnStage ~= false then partyList[#partyList + 1] = m.Uuid end
                end
            end
        end
        local out = {hero}
        local okh, hp = pcall(Probe.HostPosition)
        -- Near the hero, or near the member the player controls (members far
        -- apart; with control switched, WoW's view is round that member through Far Sight).
        local anchors = okh and {hp} or {}
        pcall(function()
            for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("ClientControl")) do
                local t = e.Transform.Transform.Translate
                anchors[#anchors + 1] = {t[1], t[2], t[3]}
            end
        end)
        for _, u in ipairs(partyList) do
            if u ~= hero and #out < 8 then
                local ok, p = pcall(function() local t = Ext.Entity.Get(u).Transform.Transform.Translate; return {t[1], t[2], t[3]} end)
                local near = false
                for _, a in ipairs(ok and p and anchors or {}) do
                    if (p[1] - a[1]) ^ 2 + (p[3] - a[3]) ^ 2 < 60 * 60 then near = true end
                end
                if near then out[#out + 1] = u end
            end
        end
        pcall(function()
            local drawn = {}
            for _, u in ipairs(out) do drawn[u] = true end
            for _, e in ipairs(Ext.Entity.GetAllEntitiesWithComponent("IsSummon")) do
                local okS, by = pcall(function() return e.IsSummon.Summoner.Uuid.EntityUuid end)
                local okO, owner = pcall(function() return e.IsSummon.Owner.Uuid.EntityUuid end)
                local okU, u = pcall(function() return e.Uuid.EntityUuid end)
                if okU and ((okS and drawn[by]) or (okO and drawn[owner])) and not drawn[u] and #out < 8 then
                    out[#out + 1] = u
                    drawn[u] = true
                end
            end
        end)
        return out
    end
    M.Figures = figures

    local function toWow(p)
        local C, W = state.Centre, state.WowOrigin
        return {W[1] - (p[3] - C[3]) / YD, W[2] + (p[1] - C[1]) / YD, W[3] + (p[2] - C[2]) / YD}
    end
    local function toBg3(w)
        local C, W = state.Centre, state.WowOrigin
        return {C[1] + (w[2] - W[2]) * YD, C[2] + (w[3] - W[3]) * YD, C[3] - (w[1] - W[1]) * YD}
    end
    M.ToWow, M.ToBg3 = toWow, toBg3
    -- Whether the session runs at all (LinkLive.lua: a fight in it); IsActive is that and not at camp
    -- (below): every module that treats BG3 as WoW's view stands aside for a trip, BG3's camp is BG3's.
    M.Running = function() return state ~= nil and state.Started == true end
    M.IsActive = function() return M.Running() and not M.InCamp() end

    local lastExtra = {}
    local function writeStatus(extra)
        if extra then lastExtra = extra end
        local s = {RunId = runId, Tick = ticks, Active = state ~= nil, Jobs = #jobs, PointerHook = M.InputHook,
            PointerEvents = M.InputEvents or 0, Clicks = clicks, RightClicks = rclicks,
            WindowClicks = M.WindowClicks or 0, LastWindow = M.LastWindow}
        for k, v in pairs(lastExtra) do s[k] = v end
        s.LevelBounds = M.LevelBounds
        Ext.IO.SaveFile("wow-bg3/world-status.json", Ext.Json.Stringify(s))
    end

    -- BG3's own mute: BG3's master volume RTPC is RTPC_Volume_MAIN (the name in BG3's
    -- binary; 0-100, its Master Volume option). An earlier version read "RTPC_Volume_Master", which
    -- BG3 doesn't have: GetRTPC answers 0 for an unknown name, so WoW stayed muted all through world
    -- mode on the desktop. Measured then in world mode: MAIN 100 while BG3 played, and
    -- still 100 with BG3's gamescope window minimized (BG3 inside gamescope never sees a focus loss,
    -- so its MuteSoundWhenNotFocused never fires and its sound keeps playing; WoW now does the same).
    -- Read every 30 ticks.
    local mutedTick, muted = nil, false
    local function bg3Muted()
        if mutedTick == nil or ticks - mutedTick >= 30 then
            mutedTick = ticks
            local ok, v = pcall(Ext.Audio.GetRTPC, "Global", "RTPC_Volume_MAIN")
            muted = ok and type(v) == "number" and v <= 0
        end
        return muted
    end

    -- Tav's position for benilla: "TAV1", seq, WoW xyz, BG3 xyz, yaw (BG3 radians), flags (1 Tav in a
    -- BG3 combat, 2 BG3's own sound muted: WoW's goes quiet with it, benilla 0030), seq2.
    -- A trip to camp (Camp.lua, switch `camp`): tav.bin keeps the spot the hero left and says "muted"
    -- (WoW's sound quiet, benilla 0030), so the WoW body stands still and the bridge holds its squares
    -- with no change on either side. A big jump is held back until Camp.lua has said what it is: one
    -- record of the camp's position through the mapping is what sent the WoW body 3.3 km away.
    local camp = {Switch = Camp.On(), Seq = 0, In = false, Last = nil, Hold = nil}
    local CAMP_JUMP, CAMP_HOLD = Camp.JUMP, Camp.PENDING + 30
    M.InCamp = function() return camp.In end
    M.CampState = camp
    local function campRead()
        local s = readJson("camp-state.json")
        if not (s and s.RunId == runId and math.type(s.Seq) == "integer" and s.Seq ~= camp.Seq) then return end
        camp.Seq = s.Seq
        if s.InCamp == true and not camp.In then
            camp.In, camp.Hold = true, nil
            -- WoW's camera stays where it was; BG3's goes to camp alone.
            -- The picking map back too: emptied, every cell reads 0, and at camp the player's region is not 0, so
            -- BG3's cursor picked nobody there (no companion to talk to).
            jobs[#jobs + 1] = {name = "camp", co = coroutine.create(function()
                arena({Action = "Lead", On = false})
                arena({Action = "PickRegionOff"}) end)}
            Ext.Log.Print("WOW-BG3 world: at camp, WoW waits")
        elseif s.InCamp ~= true and camp.In then
            camp.In, camp.Hold = false, nil
            local okp, p = pcall(Probe.HostPosition)
            -- The spot left, not the hero now: after a walk off camp the put-back has not landed yet.
            camp.Last = s.Back or (okp and p) or camp.Last
            jobs[#jobs + 1] = {name = "camp back", co = coroutine.create(function()
                arena({Action = "PickRegion", Seconds = MAX_SECONDS})
                arena({Action = "Lead", Wow = state.WowOrigin, Bg3 = state.Centre, Seconds = MAX_SECONDS}) end)}
            Ext.Log.Print("WOW-BG3 world: back from camp")
        end
    end
    local function publishTav()
        local ok, p = pcall(Probe.HostPosition)
        if not ok or not state then return end
        if camp.Switch then
            if camp.In or camp.Hold or ticks % 15 == 0 then campRead() end
            if camp.In then
                p = camp.Last or p
            elseif camp.Hold then
                if ticks < camp.Hold then return end
                camp.Hold = nil -- a long teleport, not a trip
            elseif camp.Last and (p[1] - camp.Last[1]) ^ 2 + (p[3] - camp.Last[3]) ^ 2 > CAMP_JUMP * CAMP_JUMP then
                camp.Hold = ticks + CAMP_HOLD
                return
            end
            if not camp.In then camp.Last = p end
        end
        local w = toWow(p)
        tavSeq = tavSeq + 1
        local flags = 0
        local okc, tb = pcall(function() return Hero.Entity().TurnBased end)
        if okc and tb and tb.IsInCombat_M then flags = flags | 1 end
        if bg3Muted() or camp.In then flags = flags | 2 end
        local rec = string.pack("<c4I4fffffffI4I4", "TAV1", tavSeq, w[1], w[2], w[3], p[1], p[2], p[3], 0.0, flags, tavSeq)
        Ext.IO.SaveFile("wow-bg3/link/tav.bin", rec)
        -- The character BG3 has selected, whom the camera's height follows (the plugin's pin_height).
        local oks, x, y, z = pcall(function() return Osi.GetPosition(Osi.GetHostCharacter()) end)
        if oks and x and y and z then
            Ext.IO.SaveFile("wow-bg3/link/selected.bin", string.pack("<c4I4fff", "SEL1", tavSeq, x, y, z))
        end
    end

    local function cursor()
        local ok, c = pcall(function()
            local v = Ext.UI.GetPickingHelper(1).WindowCursorPos
            return {v[1], v[2]}
        end)
        if ok and type(c[1]) == "number" and type(c[2]) == "number" then return c end
        return nil
    end
    local function publishInput(force)
        local c = cursor()
        if not c then return end
        if not force and c[1] == lastX and c[2] == lastY then return end
        lastX, lastY = c[1], c[2]
        inSeq = inSeq + 1
        Ext.IO.SaveFile("wow-bg3/link/bg3input.bin",
            string.pack("<c4I4ffffI4I4I4", "BGI2", inSeq, c[1], c[2], W, H, clicks, rclicks, inSeq))
    end
    local function wowFlags()
        local raw = Ext.IO.LoadFile("wow-bg3/link/wowhover.bin")
        if not raw or #raw < 16 then return 0 end
        local tag, seq, flags, seq2 = string.unpack("<c4I4I4I4", raw)
        return (tag == "WBH1" and seq == seq2) and flags or 0
    end
    -- BG3's own windows (a playtest report: with BG3's right-click menu or the pickpocket window open, a
    -- click went to the NPC behind it). The HUD widgets are always there; any other widget is a window
    -- (trade, pickpocket, loot, examine, inventory, ...). BG3's right-click menu is a popup outside the
    -- widget tree (harness: its widget holds the same 4 elements open or closed), so it is followed from
    -- the clicks: a right click BG3 takes with a character, corpse or item under its picking opens it, and
    -- the next left click (an entry, or closing it) is BG3's (MENU_MS at most). While either is open, a
    -- click WoW would take for a unit or an object is BG3's, and benilla does not hear of it at all
    -- (a playtest report: a click on a menu entry or the trade screen still talked to the NPC behind:
    -- the click was BG3's, but it was also forwarded and benilla replayed it on WoW's unit there);
    -- WoW's own UI keeps its clicks.
    local HUD = {WorldTooltips = true, CombatantsOverlay = true, CombatLog = true, TurnModeInfo = true,
        TargetInfo = true, Minimap = true, OverheadInfo = true, HudIndicator = true, HotBar = true,
        PassiveRoll = true, AlwaysOnTopOverlay = true, CursorText = true, PlayerPortraits = true,
        Overlay = true, NotificationWidget = true, DragAndDropPreview = true, ScreenFade = true,
        CrossplayOverlay = true, WorldContextMenu = true}
    local MENU_MS = 30000
    local menuAt = nil
    local function bg3WindowOpen()
        local open = nil
        local function walk(n, d)
            if not n or d > 6 or open then return end
            local okn, nm = pcall(function() return n.Name end)
            if n.Type == "ls.UIWidget" and okn then
                if not HUD[tostring(nm)] then open = tostring(nm) end
                return
            end
            for i = 1, math.min(n.VisualChildrenCount or 0, 256) do walk(n:VisualChild(i), d + 1) end
        end
        pcall(walk, Ext.UI.GetRoot(), 0)
        if not open and menuAt and Ext.Utils.MonotonicTime() - menuAt < MENU_MS then open = "WorldContextMenu" end
        M.LastWindow = open
        return open ~= nil
    end
    local function bg3PicksSomething()
        local ok, yes = pcall(function()
            local i = Ext.UI.GetPickingHelper(1).Inner.Inner[1]
            return i.Character ~= nil or i.DeadCharacter ~= nil or i.Item ~= nil
        end)
        return ok and yes
    end
    M.Bg3WindowOpen = bg3WindowOpen
    -- Errands (below): BG3 ignores the player's clicks while an Osiris move runs on Tav (harness
    -- tests), so a press during an errand stops her at once (stopErrandNow, set further down),
    -- before BG3 takes the click at its release.
    local stopErrandNow = function() end
    local inputHooked = false
    local function hookInput()
        if inputHooked then return end
        inputHooked = true
        -- One mouse button event (exported as M.RouteClick for the harness: SE's MouseButtonInput does
        -- not fire for injected input, so tests call it with a fake event that records PreventAction).
        local function route(e)
                M.InputEvents = (M.InputEvents or 0) + 1
                if not (state and state.Started) or M.InCamp() then return end
                local ev = e.Event or e -- SE passes the MouseButtonEvent itself (Button, Pressed)
                -- SDL buttons: 1 left (select, move), 3 right (BG3's context menu; WoW's interact)
                if ev.Button ~= 1 and ev.Button ~= 3 then return end
                local flags = wowFlags()
                local takes = flags & 3 ~= 0
                -- Not WoW's at all: a click over a BG3 window, or one the friendly-NPC rule keeps.
                local unheard = false
                if flags & 1 == 0 and bg3WindowOpen() then
                    if takes then M.WindowClicks = (M.WindowClicks or 0) + 1 end
                    takes, unheard = false, true
                end
                -- A left click on a friendly NPC (WowCursor.lua: one rule whichever pick hits it): talk or
                -- pickpocket through Interact.lua (BG3's own click prevented), or BG3's own click.
                local fc = (not unheard and ev.Button == 1 and flags & 1 == 0) and WOWBG3FriendlyClick or nil
                if fc then
                    unheard = true
                    takes = fc.Mode == "talk" or fc.Mode == "pickpocket"
                    if takes and ev.Pressed == false then
                        M.FriendlySeq = math.max((M.FriendlySeq or 0) + 1, math.floor(Ext.Utils.MonotonicTime()))
                        Ext.IO.SaveFile("wow-bg3/link/friendlyclick.txt",
                            string.format("%d %s %s %s\n", M.FriendlySeq, fc.Mode, fc.Guid or "0", fc.Uuid))
                        M.FriendlyClicks = (M.FriendlyClicks or 0) + 1
                        M.LastFriendly = fc.Mode
                    end
                end
                -- A right click BG3 takes for its menu (RightAttack.lua: on a character BG3 picks) is BG3's
                -- alone, also where WoW picks nothing there (harness, real clicks: in Razor Hill's huts
                -- WoW's camera sees the roof, picked no unit, and the right click went on to WoW as well).
                local bg3Menu = not unheard and ev.Button == 3 and WOWBG3BG3TakesClick == true
                if bg3Menu then takes, unheard = false, true end
                -- BG3's right-click menu (see bg3WindowOpen): opened by a right click BG3 takes on something
                -- it picks, closed by the next left click.
                if ev.Pressed == false then
                    if ev.Button == 3 and (bg3Menu or (not takes and bg3PicksSomething())) then
                        menuAt = Ext.Utils.MonotonicTime()
                    elseif ev.Button == 1 then
                        menuAt = nil
                    end
                end
                -- BG3 attacks a friendly NPC (see the header): its click, not WoW's.
                local bg3Attack = takes and WOWBG3BG3TakesClick == true
                if bg3Attack then
                    takes = false
                    M.FriendlyAttackClicks = (M.FriendlyAttackClicks or 0) + 1
                end
                if ev.Pressed == true then pcall(stopErrandNow) end
                if ev.Pressed == false and not bg3Attack and not unheard then
                    if ev.Button == 1 then clicks = clicks + 1 else rclicks = rclicks + 1 end
                    publishInput(true)
                end
                if takes then pcall(function() e:PreventAction() end) end
        end
        M.RouteClick = route
        WOWBG3World = M -- for the harness's research chunks (Ext.Require does not resolve from them)
        local ok, err = pcall(function()
            Ext.Events.MouseButtonInput:Subscribe(route)
        end)
        M.InputHook = ok and "MouseButtonInput" or ("none: " .. tostring(err))
        Ext.Log.Print("WOW-BG3 world: pointer hook " .. M.InputHook)
    end

    -- Errands (see the header). placed: pool subgrid K -> {MinX, MinZ} of the 64 m square it was
    -- last placed at (Start and Stream).
    local placed, goRaw, errandSeq, errandUntil = {}, nil, 0, nil
    local function notePlaced(cells)
        for _, cell in ipairs(cells or {}) do
            local k, minX, minZ = math.tointeger(cell[1]), tonumber(cell[2]), tonumber(cell[3])
            if k and minX and minZ then placed[k] = {minX, minZ} end
        end
    end
    local function onPlacedGround(p)
        for _, sq in pairs(placed) do
            if p[1] >= sq[1] + 0.5 and p[1] <= sq[1] + 63.5 and p[3] >= sq[2] + 0.5 and p[3] <= sq[2] + 63.5 then
                return true
            end
        end
        return false
    end
    local function sendErrand(e)
        errandSeq = errandSeq + 1
        e.RunId, e.Seq = runId, errandSeq
        Ext.IO.SaveFile("wow-bg3/errand.json", Ext.Json.Stringify(e))
        lastExtra.Errand = e
        -- While one may be running (StandIns.lua gives up after 20 s), a press stops it.
        errandUntil = e.Go == true and ticks + 20 * 60 or nil
    end
    stopErrandNow = function()
        if errandUntil and ticks < errandUntil then
            sendErrand({Go = false, Stop = true, Why = "BG3 click"})
        end
    end
    local function readErrand()
        local raw = Ext.IO.LoadFile("wow-bg3/link/tavgo.bin") or ""
        -- The record there at the first look is from before this session; the same bytes again are
        -- no news.
        if goRaw == nil then goRaw = raw; return end
        if raw == goRaw or #raw < 40 then return end
        local tag, seq, lo, hi, x, y, z, stop, flags, seq2 = string.unpack("<c4I4I4I4ffffI4I4", raw)
        if tag ~= "TGO1" or seq ~= seq2 then return end
        goRaw = raw
        local guid = string.format("%08x%08x", hi, lo)
        if flags & 1 == 0 then
            sendErrand({Go = false, Stop = flags & 2 ~= 0, Guid = guid})
            return
        end
        local okp, p = pcall(Probe.HostPosition)
        if not okp then return end
        local t = toBg3({x, y, z})
        local dx, dz = p[1] - t[1], p[3] - t[3]
        local d, s = math.sqrt(dx * dx + dz * dz), math.max(0.0, stop) * YD
        local dest = d > s and {t[1] + dx / d * s, t[2], t[3] + dz / d * s} or {p[1], p[2], p[3]}
        local okc, tb = pcall(function() return Hero.Entity().TurnBased end)
        local why
        if okc and tb and tb.IsInCombat_M then why = "in combat"
        elseif not onPlacedGround(dest) then why = "not on placed ground"
        elseif d > 80 then why = "too far" end
        if why then
            Ext.Log.Print("WOW-BG3 world: errand to " .. guid .. " refused: " .. why)
            sendErrand({Go = false, Stop = false, Guid = guid, Refused = why})
            return
        end
        sendErrand({Go = true, Guid = guid, Position = dest, Target = t, Distance = d})
    end

    local function start(c)
        local function vec(t, n)
            if type(t) ~= "table" or #t ~= n then return false end
            for i = 1, n do if type(t[i]) ~= "number" or t[i] ~= t[i] then return false end end
            return true
        end
        assert(vec(c.Centre, 3) and vec(c.WowOrigin, 3) and type(c.Cells) == "table" and #c.Cells >= 1,
            "Centre, WowOrigin and Cells required")
        -- Only in a game started with WOW-BG3 (WorldFlag.lua): an old save
        -- loaded with the mod enabled stays plain BG3. The harness sets WOW_BG3_WORLD_FLAG=1 for its saves.
        local okFlag, flagOn = pcall(function() return Ext.Require("WorldFlag.lua").On() end)
        assert(okFlag and flagOn == true, "world mode is off for this game (WorldFlag: "
            .. (okFlag and tostring(flagOn) or tostring(flagOn)) .. ")")
        state = {Centre = c.Centre, WowOrigin = c.WowOrigin, Layer = tonumber(c.Layer) or -15.262, At = ticks}
        camp.Last, camp.Hold = nil, nil -- a new session's teleport is no trip
        -- Several out-of-bounds layers (plugin 0.2.9): Layers lists their heights.
        if type(c.Layers) == "table" and #c.Layers > 0 then state.Layers = c.Layers end
        -- The triangles of a 3 x 2 window of squares span 192 m: raise the plugin's span limit. Since plugin
        -- 0.2.9 the limit is per cluster of touching squares (members far apart are separate clusters);
        -- members near each other merge into one cluster, so big pools get 1024 m (0.2.8 keeps its 400).
        local poolSize = math.tointeger(c.PoolSize) or 6
        local lim = arena({Action = "Limits", Side = 0, Span = poolSize > 12 and 1024 or 300})
        if poolSize > 12 and type(lim) == "table" and (tonumber(lim.Span) or 0) < 1024 then
            arena({Action = "Limits", Side = 0, Span = 400}) -- plugin 0.2.8 refuses more than 400
        end
        -- The pool: whole plain subgrids of the out-of-bounds layer(s) that both sides qualify (the
        -- server's grid has things standing on some the client's has not), the same ids on both.
        arena({Action = "Pool", Layer = state.Layer, Layers = state.Layers, Count = math.max(12, poolSize)})
        local server, client = readJson("native-server-pool.json") or {}, readJson("native-client-pool.json") or {}
        local onClient, ids = {}, {}
        for _, e in ipairs(client) do onClient[math.tointeger(e.Id) or -1] = true end
        for _, e in ipairs(server) do
            local id = math.tointeger(e.Id)
            if id and onClient[id] and #ids < (math.tointeger(c.PoolSize) or 6) then ids[#ids + 1] = string.format("%d", id) end
        end
        assert(#ids >= 4, "fewer than 4 out-of-bounds subgrids qualify on both sides")
        Ext.IO.SaveFile("wow-bg3/pool-ids.txt", table.concat(ids, " "))
        arena({Action = "Pool", Layer = state.Layer, Layers = state.Layers, Count = #ids, Ids = true})
        -- BG3's cursor picks a character only where the level's region map matches the player's
        -- region: the map is emptied on the client, so Tav and a fight's stand-ins pick anywhere in
        -- the void (Arena.lua PickRegion). A refusal (another BG3 build) leaves picking as BG3 has it.
        -- The level's own AI grid bounds before any square moves (metres: x0, z0, x1, z1), for the bridge:
        -- a party member standing in the level itself never gets treadmill squares there.
        local okB, b = pcall(arena, {Action = "Bounds"})
        if okB and type(b) == "table" and type(b.Min) == "table" and type(b.Max) == "table" then
            M.LevelBounds = {b.Min[1] * 0.5, b.Min[2] * 0.5, b.Max[1] * 0.5, b.Max[2] * 0.5}
        end
        local okPick, errPick = pcall(arena, {Action = "PickRegion", Seconds = MAX_SECONDS})
        if not okPick then Ext.Log.Print("WOW-BG3 world: picking region map left alone: " .. tostring(errPick)) end
        arena({Action = "Place", Cells = c.Cells, Seconds = MAX_SECONDS})
        placed = {}
        notePlaced(c.Cells)
        arena({Action = "Rays", Void = true, Mask = 2096, SkipContexts = 0, Seconds = MAX_SECONDS})
        -- The hero at Start goes to the void; Stop brings that one back (Hero.lua: the controlled character).
        state.Hero = Hero.Assert()
        standIn({Action = "Teleport", Target = state.Hero, Position = {c.Centre[1], c.Centre[2] + 1, c.Centre[3]}})
        wait(30)
        pcall(standIn, {Action = "Away"})
        -- The hero's companions come along: next to the hero and chained, so BG3's own
        -- party following moves them on the treadmill; Party.lua's upkeep keeps stragglers on its ground.
        local okParty, errParty = pcall(party, {Action = "Gather", Upkeep = true,
            Position = {c.Centre[1], c.Centre[2] + 1, c.Centre[3]}})
        if not okParty then Ext.Log.Print("WOW-BG3 world: party not gathered: " .. tostring(errParty)) end
        -- BG3's game over is no longer held here: Death.lua owns death by BG3's
        -- rules (party-multiplayer-design.md "Death") and undoes any such hold.
        -- BG3's exploration music off for the session, after the teleport (see the notes
        -- at the top): WoW plays its own music; BG3's combat music still plays in its fights.
        local okMusic, errMusic = pcall(standIn, {Action = "Music", Event = "Music_Off"})
        if not okMusic then Ext.Log.Print("WOW-BG3 world: BG3's exploration music left on: " .. tostring(errMusic)) end
        arena({Action = "Lead", Wow = c.WowOrigin, Bg3 = c.Centre, Seconds = MAX_SECONDS})
        state.Started = true
        hookInput()
        Ext.Log.Print("WOW-BG3 world: started at " .. Ext.Json.Stringify(c.Centre))
    end
    local function stream(c)
        assert(state and state.Started, "world mode not started")
        if type(c.Cells) == "table" and #c.Cells > 0 then
            -- A square moves on both sides or on neither. Place refuses a pool subgrid
            -- that something stands on, and each side decides alone, so a refusal on one side used to leave
            -- the other side's square moved: the server then had no ground where Tav walked on (no path,
            -- no surfaces under her, her height off the tiles), while the client showed it. Both sides
            -- answer first; a busy subgrid stays where it is and the bridge fills that square from
            -- another one (world_bridge.py: "stands on pool K").
            local okc, why, got = pcall(arena, {Action = "PlaceCheck", Cells = c.Cells, Seconds = MAX_SECONDS})
            if not okc then Ext.Log.Print("WOW-BG3 world: place check unavailable (Place decides alone): " .. tostring(why)) end
            local busy, list = {}, {}
            for _, r in pairs(okc and got or {}) do
                for _, k in ipairs(type(r.Value) == "table" and type(r.Value.Busy) == "table" and r.Value.Busy or {}) do
                    if not busy[k] then busy[k] = true; list[#list + 1] = k end
                end
            end
            if #list > 0 then
                table.sort(list)
                error("Place refused: something stands on pool " .. table.concat(list, " "), 0)
            end
            arena({Action = "Place", Cells = c.Cells, Seconds = MAX_SECONDS})
            notePlaced(c.Cells)
        end
        if c.Rays ~= false then
            arena({Action = "Rays", Void = true, Mask = 2096, SkipContexts = 0, Seconds = MAX_SECONDS})
        end
    end
    local function stop()
        if not state then return end
        -- Stop publishing first: Tav is sent back to her BG3 position below, and benilla would
        -- otherwise walk the WoW character toward that spot's mapping while Stop runs.
        state.Started = false
        local problems = {}
        for _, step in ipairs({
            function() arena({Action = "Lead", On = false}) end,
            function() standIn({Action = "ClearSurfaces", Target = state.Hero or Hero.Uuid()}) end,
            function() standIn({Action = "Back"}) end,
            function() standIn({Action = "Return", Target = state.Hero or Hero.Uuid()}) end,
            function() wait(30) end,
            -- The companions go with the hero, before the void's ground goes home.
            function() party({Action = "Gather", Upkeep = false}) end,
            -- Every stand-in gone before the ground goes home (tele.sh's failed Start):
            -- one still standing on a square marks that subgrid's home tiles taken, and it never
            -- qualifies for the pool again in this BG3 session.
            function()
                local centre = state.Centre
                for i = 1, 20 do
                    local r = standIn({Action = "Clear", Centre = centre, Evict = i >= 6})
                    if r.Left == 0 then
                        -- What still stands in the void as the ground goes home (wow-bg3/stop-report.json).
                        Ext.IO.SaveFile("wow-bg3/stop-report.json", Ext.Json.Stringify({RunId = runId, Tick = ticks, Near = r.Near}))
                        return
                    end
                    wait(10)
                end
                error("stand-ins still there after Clear")
            end,
            function() arena({Action = "Restore"}) end,
        }) do
            local ok, err = pcall(step)
            if not ok then problems[#problems + 1] = tostring(err) end
        end
        state = nil
        pcall(TavPlan.Clear)
        pcall(TavPlan.ClearKeep)
        return {Problems = problems}
    end

    -- Research (geometry copy, harness only): runs wow-bg3/research.lua (a chunk returning a
    -- function of M) on the client and writes what it returns to wow-bg3/research-result.json.
    local function research()
        assert(Harness.On, "harness actions are off (WOW_BG3_HARNESS=1)")
        local src = Ext.IO.LoadFile("wow-bg3/research.lua")
        assert(src, "no research.lua")
        local chunk = assert(load(src))
        local ok, value = pcall(chunk(), M)
        Ext.IO.SaveFile("wow-bg3/research-result.json", Ext.Json.Stringify({RunId = runId, OK = ok, Value = value}))
    end

    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if not Session.Live() then -- Running, or held under BG3's loading screen
            if Session.Held() then return end -- a save: same session, keep world mode
            if state then state = nil; jobs = {}; pcall(TavPlan.Clear); pcall(TavPlan.ClearKeep) end
            return
        end
        -- Run queued jobs (coroutines), one at a time.
        local job = jobs[1]
        if job then
            local ok, err = coroutine.resume(job.co)
            if not ok or coroutine.status(job.co) == "dead" then
                table.remove(jobs, 1)
                -- Seq: the bridge waits for its command's report before sending the next one.
                writeStatus({Last = job.name, Seq = job.seq, OK = ok, Error = ok and nil or tostring(err)})
                if not ok then Ext.Log.Print("WOW-BG3 world " .. job.name .. " failed: " .. tostring(err)) end
            end
        end
        if state and state.Started then
            if ticks % 3 == 0 then publishTav() end
            if ticks % 3 == 1 and not M.InCamp() then
                local ok, err = pcall(readErrand)
                if not ok then lastExtra.ErrandError = tostring(err) end
            end
            if not M.InCamp() then publishInput(false) end
            if ticks % 600 == 0 then writeStatus() end
            -- Tav's meshes and poses for WoW (TavPlan.lua), checked every TavPlan.Every ticks (since
            -- Every 10, was 30: a weapon drawn or an attachment of a cast reaches WoW
            -- sooner; a check costs TavPlan.LastMs, in world-status.json as TavMs).
            if ticks % TavPlan.Every == 7 % TavPlan.Every and not M.InCamp() then
                -- The hero and the companions near her (one figure each).
                local ok, n = TavPlan.Publish(figures(), state.Centre, state.WowOrigin)
                lastExtra.TavMeshes = ok and n or ("error: " .. tostring(n))
                lastExtra.TavMs, lastExtra.TavPlans = TavPlan.LastMs, TavPlan.Plans
            end
            -- The characters BG3 keeps drawing with its map off (compose word "void"): those within
            -- 60 m of Tav (her, a fight's stand-ins).
            if ticks % 30 == 22 and not M.InCamp() then
                local okp, p = pcall(Probe.HostPosition)
                if okp then
                    local ok, n = pcall(TavPlan.PublishKeep, p, 60, figures())
                    lastExtra.Kept = ok and n or ("error: " .. tostring(n))
                end
            end
            if ticks - (state.Extended or state.At) > 60 * 240 and #jobs == 0 then
                state.Extended = ticks
                jobs[#jobs + 1] = {name = "extend", co = coroutine.create(function()
                    arena({Action = "Extend", Seconds = MAX_SECONDS})
                end)}
            end
        end
        if ticks % 15 ~= 0 then return end
        local c = readJson("world-command.json")
        if not c or c.RunId ~= runId or math.type(c.Sequence) ~= "integer" or c.Sequence <= sequence then return end
        sequence = c.Sequence
        local fn = ({Start = start, Stream = stream, Stop = stop, Research = research})[c.Action]
        if not fn then writeStatus({Last = tostring(c.Action), OK = false, Error = "unknown action"}); return end
        jobs[#jobs + 1] = {name = c.Action, seq = c.Sequence, co = coroutine.create(function() return fn(c) end)}
    end)
end
return M
