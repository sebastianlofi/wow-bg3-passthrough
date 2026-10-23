-- Right-click in world mode (client context). Design (party-multiplayer-design.md,
-- Interaction): a right-click on a WoW mob or NPC opens BG3's own context menu (Examine, Talk,
-- Trade, Attack...), as BG3 does for its own characters; a left-click on a WoW NPC talks to it
-- (WoW's interact through the pointer link, benilla 0007/0019: vendors talk, then trade; errands
-- walk BG3's character there first). Previously a right-click on a mirrored mob made the
-- hero's basic attack (M.Mode = "Attack" keeps that, research knob).
--
-- How: BG3 can only show its menu for a BG3 entity, so the right press goes to BG3 whenever
-- BG3's own picking has a character under the cursor: a living stand-in of a WoW mob or NPC
-- (Mirrors.lua, link/mirror-status.json) or a party member. WorldMode.lua sends a click to WoW
-- when WoW reports a unit there (wowhover.bin flags 1 or 2, a friendly NPC's stand-in included)
-- unless the mod-wide WOWBG3BG3TakesClick is set when its MouseButtonInput handler runs. This
-- module subscribes ahead of it (Priority 200, SE's default is 100) and sets that flag for every
-- mouse button event: right button over a picked BG3 character -> true (BG3's menu, WoW never
-- hears of the click); left button -> WowCursor.lua's own choice (BG3 attacks a friendly NPC with
-- Ctrl or an aimed attack). Everything else stays as it was: a right-click on a WoW NPC with
-- no stand-in (far away), a corpse, a WoW object or WoW's UI is WoW's (interact, loot, errands).
-- BG3's menu there (harness): Attack and Examine on a mob; Talk, Attack and Examine on a
-- friendly NPC (Interact.lua gives its stand-in BG3's dialog flag and sends Talk on to WoW);
-- Examine is BG3's own window (the stand-in shows the WoW name and level, Names.lua, Portraits.lua).
-- Report: link/right-click.json (the last routed press) and M.Last.
local M = {}
local World = Ext.Require("WorldMode.lua")
local Hero = Ext.Require("Hero.lua")
local Cursor = Ext.Require("WowCursor.lua")
local MELEE, RANGED = "Target_MainHandAttack", "Projectile_MainHandAttack"
M.Mode = "Menu" -- "Menu" (BG3's context menu) or "Attack" (old: basic attack on a mirrored mob)
M.Last = nil
M.Routed = {Menu = 0, Wow = 0, Left = 0}
-- The module for the harness's research chunks (Ext.Require does not resolve from them).
WOWBG3Right = M

local function findUnique(widget, name)
    local found, seen, visited = nil, {}, 0
    local function walk(node, owner, depth)
        if not node or depth > 24 or visited >= 4096 or seen[tostring(node)] then return end
        seen[tostring(node)] = true
        visited = visited + 1
        local ok, nodeName = pcall(function() return node.Name end)
        if not ok then nodeName = nil end
        if node.Type == "ls.UIWidget" then owner = nodeName end
        if owner == widget and nodeName == name then found = node end
        for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), owner, depth + 1) end
    end
    walk(Ext.UI.GetRoot(), nil, 0)
    return found
end

-- Stand-ins (link/mirror-status.json), refreshed at most every 0.5 s: uuid -> {Guid, Friendly, Dead}.
local standIns, standInsAt = {}, nil
local function refresh()
    local now = Ext.Utils.MonotonicTime()
    if standInsAt and now - standInsAt < 500 then return standIns end
    standInsAt = now
    local raw = Ext.IO.LoadFile("wow-bg3/link/mirror-status.json")
    local ok, s = pcall(Ext.Json.Parse, raw or "")
    local set = {}
    if ok and type(s) == "table" and type(s.Mobs) == "table" then
        for guid, m in pairs(s.Mobs) do
            if type(m) == "table" and type(m.Uuid) == "string" then
                set[m.Uuid] = {Guid = guid, Friendly = m.Friendly == true, Dead = m.Dead == true}
            end
        end
    end
    standIns = set
    return set
end
M.StandIns = function() standInsAt = nil; return refresh() end

-- The character BG3's cursor picks, and the cursor position.
local function picked()
    local p = Ext.UI.GetPickingHelper(1)
    local c = p.WindowCursorPos
    local inner = p.Inner.Inner[1]
    local uuid = inner and inner.Character and tostring(inner.Character.Uuid.EntityUuid) or nil
    return uuid, math.floor(c[1] + 0.5), math.floor(c[2] + 0.5)
end

-- Whether BG3 takes a right press over this pick (see the header). Returns the reason or nil.
function M.MenuFor(uuid)
    if not uuid then return nil end
    local s = refresh()[uuid]
    if s then
        if s.Dead then return nil end -- a WoW corpse: WoW's loot (inventory brings BG3's later)
        return s.Friendly and "npc" or "mob"
    end
    -- Any other BG3 character: the party (the hero included) and BG3's own characters.
    local ok, e = pcall(Ext.Entity.Get, uuid)
    if ok and e then return uuid == Hero.Uuid() and "hero" or "character" end
    return nil
end

local function basicAttack(dc)
    local props = dc.CurrentPlayer.SelectedCharacter.PlayerCharacterProperties
    local slots = {}
    for _, bar in ipairs({"CommonHotBar", "ClassHotBar"}) do
        local list = props[bar].SlotList
        for i = 1, 64 do
            local s = list[i]
            if s == nil then break end
            local id = s.Content and s.Content.PrototypeID
            if id == MELEE or id == RANGED then slots[id] = slots[id] or s end
        end
    end
    local m, r = slots[MELEE], slots[RANGED]
    if m and m.CanUse == true then return m, MELEE end
    if r and r.CanUse == true then return r, RANGED end
    return nil
end

local function report(runId, job)
    M.Last = job
    Ext.IO.SaveFile("wow-bg3/link/right-click.json", Ext.Json.Stringify({RunId = runId, Job = job, Routed = M.Routed}))
end

-- One mouse button event, before WorldMode.lua's handler (exported for the harness: SE's
-- MouseButtonInput does not fire for injected input, so tests call this with a fake event).
function M.Route(ev, uuid)
    if ev.Button == 1 then
        WOWBG3BG3TakesClick = Cursor.FriendlyAttack == true
        if ev.Pressed == true then M.Routed.Left = M.Routed.Left + 1 end
        return "left"
    end
    if ev.Button ~= 3 then return nil end
    local why = M.MenuFor(uuid)
    WOWBG3BG3TakesClick = why ~= nil
    if ev.Pressed == true then
        if why then M.Routed.Menu = M.Routed.Menu + 1 else M.Routed.Wow = M.Routed.Wow + 1 end
    end
    return why and ("bg3:" .. why) or "wow"
end

function M.Start(runId)
    local ticks, pending = 0, nil
    local ok, err = pcall(function()
        Ext.Events.MouseButtonInput:Subscribe(function(e)
            if not (World.IsActive and World.IsActive()) then return end
            local ev = e.Event or e -- SE passes the MouseButtonEvent itself (Button, Pressed)
            local okp, uuid, x, y = pcall(picked)
            if not okp then uuid = nil end
            if M.Mode == "Attack" and ev.Button == 3 then
                -- Old behaviour (research knob): the hero's basic attack on a mirrored mob.
                local s = uuid and refresh()[uuid]
                if not s or s.Dead or s.Friendly or uuid == Hero.Uuid() then return end
                pcall(function() e:PreventAction() end)
                if ev.Pressed ~= false and not pending then
                    pending = {Uuid = uuid, X = x, Y = y, At = ticks, Stage = "slot"}
                end
                return
            end
            local route = M.Route(ev, uuid)
            if ev.Button == 3 and ev.Pressed == true then
                report(runId, {Tick = ticks, Uuid = uuid, X = x, Y = y, Route = route,
                    Guid = uuid and refresh()[uuid] and refresh()[uuid].Guid or nil})
            end
        end, {Priority = 200})
    end)
    M.Hook = ok and "MouseButtonInput" or ("none: " .. tostring(err))
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        local job = pending
        if not job then return end
        local okj, errj = pcall(function()
            if job.Stage == "slot" and ticks - job.At >= 2 then
                local dc = findUnique("HotBar", "HotBar").DataContext
                assert(dc.CurrentPlayer.SelectedCharacter.EntityUUID == Hero.Uuid(), "The hero is not selected")
                local slot, id = basicAttack(dc)
                assert(slot, "no basic attack can be used now")
                assert(dc.UseSlotCommand:CanExecute(slot), id .. " cannot execute")
                dc.UseSlotCommand:Execute(slot)
                job.Spell, job.Stage, job.At = id, "click", ticks
            elseif job.Stage == "click" and ticks - job.At >= 8 then
                local uuid, x, y = picked()
                assert(uuid == job.Uuid, "the cursor left the creature")
                Ext._Internal.InjectMouse(1, 1, true, x, y)
                job.Stage, job.At, job.X, job.Y = "release", ticks, x, y
            elseif job.Stage == "release" and ticks - job.At >= 3 then
                Ext._Internal.InjectMouse(1, 1, false, job.X, job.Y)
                job.Stage = "done"
            end
        end)
        if not okj then job.Error, job.Stage = tostring(errj), "done" end
        if job.Stage == "done" then
            pending = nil
            report(runId, job)
        end
    end)
end
return M
