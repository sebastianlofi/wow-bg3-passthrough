-- WoW's cursor meaning on BG3's cursor (world mode, step 1: BG3's own cursor
-- types; client context). benilla writes what WoW has under the pointer into link/wowhover.bin:
-- "WBH1", seq, flags (1 WoW's UI, 2 a WoW unit or object WoW takes the click for), seq again, then
-- (patch 0009) WoW's cursor kind, unable (grayed), seq a third time. While WoW takes the pointer
-- for a unit or object, BG3's cursor shows the closest BG3 cursor type: Talk for speaking, quests,
-- vendors, trainers and flight masters, Melee or Bow for creatures, OpenContainer for loot,
-- Shovel for mining and herbs, Lock for locks, ItemUse for other objects; the grayed WoW cursor is
-- BG3's Warning twin.
-- Mobs with a BG3 stand-in: BG3 takes the click (its stand-in), but its
-- own cursor over a neutral stand-in is the plain arrow, so the sword showed only over far mobs
-- (no stand-in yet). benilla now sets flag 4 for a mirrored mob under the pointer, with WoW's
-- cursor kind and gray state for it: BG3 shows Melee (MeleeWarning when WoW's is grayed). BG3
-- picking a stand-in counts too (WoW's model and the stand-in are not always under the same
-- pixel: the stand-in follows the mob 0.35 m at a time, and is often smaller than WoW's model).
-- Now BG3's pick finds the stand-in wherever WoW has its unit under the pointer
-- (StandInAim.lua, plugin op 32): every BG3 action aimed at it (attack, spell, shove, throw, talk,
-- pickpocket) is BG3's own, on WoW's model as on the stand-in. Over a stand-in BG3 writes its own
-- RequestedCursor (Arrow for a neutral one) every frame after this module's tick (harness
-- tests: RequestedCursor read back "Arrow", CursorName "CursorArrow"), so that case uses
-- CursorOverrides, and only while BG3's own choice there is the plain Arrow: a spell or throw
-- being aimed, or a hostile stand-in in a fight, keeps BG3's cursor. CursorOverrides takes the
-- CursorType numbers (bg3le refuses the names: "number expected, got string"; Melee is 28).
-- Friendly NPCs: a vendor, quest giver, guard or villager with a
-- stand-in stays WoW's to click (benilla 0027 keeps flag 2: talk, trade, quests, errands) and shows
-- WoW's cursor, through CursorOverrides as BG3 rewrites its own choice over a picked stand-in. When
-- BG3's own choice there is an attack (BG3 lets the player attack someone who is not hostile with
-- Ctrl held, or with an attack or spell aimed at them), BG3's cursor shows and the click is BG3's
-- (WOWBG3BG3TakesClick, read by WorldMode.lua): BG3's own flow for that attack follows (Mirrors.lua).
-- WoW's quest cursor (party-multiplayer-design.md: cursors are BG3's own
-- except over a WoW quest giver, which shows WoW's gold ! for a quest to pick up and gold ? for one to
-- hand in). Two BG3 cursor types world mode never uses, Listen and ListenWarning, show WoW's art:
-- BG3 builds its SDL cursors from Game.pak's Cursors/*.cur before any plugin or Lua runs (harness:
-- an override of the files came too late), so the bg3le plugin wowbg3-early (plugins/early)
-- hooks BG3's SDL_SetCursor and swaps BG3's Listen cursors for SDL cursors made from
-- the mod's IO folder's cursors/quest-*.cur (Mods/WOW-BG3/data/io) (WoW's Interface\GossipFrame\AvailableQuestIcon.blp
-- and ActiveQuestIcon.blp of the player's WoW; link/tools/wowcursor.py, or benilla in the release).
-- Used only when that plugin is loaded and the four files are there (otherwise the quest giver keeps
-- BG3's Talk cursor). The quest state comes from benilla: wowhover.bin's 36-byte form adds WoW's
-- questgiver dialog status of the unit under the pointer (SMSG_QUESTGIVER_STATUS: 5 available, 6 and
-- 7 reward, 4 repeatable reward; 1 and 3, the grey marks, keep Talk) and the seq a fourth time.
-- One rule for a left click on a friendly NPC (a playtest report: on its model nothing happened, a
-- little beside it the cursor showed a steal and the click talked): WoW picks its model, BG3 picks the
-- stand-in, and the two do not cover the same pixels, so the NPC under the pointer is whichever of
-- them picks one (WoW's first: the model is what the player sees; wowhover.bin's 48-byte form names
-- the unit, benilla 0067), and the click does the same either way (WOWBG3FriendlyClick, read by
-- WorldMode.lua):
--   sneaking (the hero has eoc::sneak::IsSneakingComponent): pickpocket, BG3's PickPocket cursor and BG3's
--     own click (BG3 picks the stand-in under WoW's model too, StandInAim.lua); where even that finds
--     no stand-in (hidden from BG3's camera) the click does nothing;
--   walking, an NPC WoW has something for (a service in UNIT_NPC_FLAGS, benilla's wowbody/<guid>.txt): talk,
--     WoW's cursor for it (Talk, the merchant or quest cursor) and WoW's interact (Interact.lua);
--   walking, an NPC with nothing to say: no Talk cursor, BG3's plain click (Tav walks there).
-- An attack aimed at it (Ctrl, a spell) stays BG3's, as before. While a BG3 window is open (trade,
-- pickpocket, examine, BG3's right-click menu) nothing here applies: the pointer is BG3's window's.
-- Report: wow-bg3/cursor-status.json.
local M = {}
local World = Ext.Require("WorldMode.lua")
local Hero = Ext.Require("Hero.lua")
local Aim = Ext.Require("StandInAim.lua")
local N = Ext.Require("WowNative.lua")
-- benilla's CursorKind order (target/cursor_mode.rs).
local KINDS = {[0] = "Point", "Attack", "Speak", "Pickup", "LootAll", "Interact", "Buy", "Inspect", "Trainer",
    "Taxi", "Skin", "Repair", "Mail", "Mine", "GatherHerbs", "PickLock", "Cast"}
local BG3 = {Attack = "Melee", Speak = "Talk", Pickup = "ItemPickup", LootAll = "OpenContainer",
    Interact = "ItemUse", Buy = "Talk", Inspect = "ItemUse", Trainer = "Talk", Taxi = "Talk", Skin = "ItemUse",
    Repair = "ItemUse", Mail = "OpenContainer", Mine = "Shovel", GatherHerbs = "Shovel", PickLock = "Lock",
    Cast = "Cast"}
-- Research knob: "Requested" writes RequestedCursor each tick, "Override" keeps one entry in
-- CursorOverrides while it applies. MirroredMode: the same for a mirrored mob (flag 4), where
-- BG3's own picking of the stand-in may ask for its cursor every frame.
M.Mode = "Requested"
M.MirroredMode = "Override"
M.Last = nil
M.Flags = nil
-- The BG3 cursor types shown for WoW's quest marks, and which dialog status asks for which.
M.QuestCursors = {Available = "Listen", TurnIn = "ListenWarning"}
local QUEST = {[5] = "Available", [4] = "TurnIn", [6] = "TurnIn", [7] = "TurnIn"}
M.QuestInstalled = false
M.Quest = nil
-- This tick's choice for a left click over a friendly NPC (RightAttack.lua sets the mod-wide flag
-- per button from it).
M.FriendlyAttack = false

-- CursorType name -> number, for CursorOverrides.
local ids = nil
local function cursorId(name)
    if not ids then
        ids = {}
        for i = 0, 127 do
            local ok, n = pcall(function() return Ext.Enums.CursorType[i] end)
            if ok and n ~= nil then ids[tostring(n)] = i end
        end
    end
    return ids[name]
end

-- Stand-ins of mirrored mobs (link/mirror-status.json, Mirrors.lua), read every 30 ticks; which of
-- them are friendly NPCs (uuid -> guid, and guid -> uuid); every living one by guid (StandInAim.lua).
local standIns, friendlies, friendlyGuids, byGuid, standInsAt = {}, {}, {}, {}, nil
local function refreshStandIns(ticks)
    if standInsAt and ticks - standInsAt < 30 then return end
    standInsAt = ticks
    local raw = Ext.IO.LoadFile("wow-bg3/link/mirror-status.json")
    local ok, st = pcall(Ext.Json.Parse, raw or "")
    local set, fr, frg, all = {}, {}, {}, {}
    if ok and type(st) == "table" and type(st.Mobs) == "table" then
        for g, m in pairs(st.Mobs) do
            if type(m) == "table" and type(m.Uuid) == "string" and not m.Dead then
                set[m.Uuid] = true
                all[g:lower()] = m.Uuid
                if m.Friendly == true then fr[m.Uuid] = g:lower(); frg[g:lower()] = m.Uuid end
            end
        end
    end
    standIns, friendlies, friendlyGuids, byGuid = set, fr, frg, all
end
-- WoW's UNIT_NPC_FLAGS of a mirrored unit (benilla's wowbody/<guid>.txt), cached; nil until written.
local npcFlags = {}
-- A quest giver with no other service and nothing for us (WoW's dialog status 0 none, 1 grey: not yet
-- available) has nothing to say: WoW's right click opens nothing there (a playtest report: Talk on NPCs
-- without dialog; harness: quest givers with status 0 and 1 showed Talk). The status comes with WoW's
-- hover (wowhover.bin); each one seen is kept per guid and written to link/questsilent.txt ("<guid>
-- <status>" lines) for Interact.lua, which then takes Talk off BG3's menu for them.
local questStatus, questWritten = {}, nil
local function noteQuest(guid, st)
    if not guid or math.type(st) ~= "integer" or questStatus[guid] == st then return end
    questStatus[guid] = st
    local lines = {}
    for g, v in pairs(questStatus) do lines[#lines + 1] = g .. " " .. v end
    table.sort(lines)
    local text = table.concat(lines, "\n") .. "\n"
    if text ~= questWritten then
        questWritten = text
        Ext.IO.SaveFile("wow-bg3/link/questsilent.txt", text)
    end
end
M.QuestStatus = questStatus
local function talks(guid)
    local f = npcFlags[guid]
    if f == nil then
        local text = Ext.IO.LoadFile("wow-bg3/link/wowbody/" .. guid .. ".txt")
        f = text and tonumber(text:match("npcflags (%d+)")) or false
        if f ~= false then npcFlags[guid] = f end
    end
    if f == false then return nil end
    local services = f & 0x7fff -- 1.12's services (gossip to repair), as Interact.lua
    if services == 2 then
        local st = questStatus[guid]
        if st == 0 or st == 1 then return false end
    end
    return services ~= 0
end
-- The hero sneaks (BG3's client has the sneaking tag component on her then).
local function sneaking()
    local ok, yes = pcall(function()
        local e = Ext.Entity.Get(Hero.Uuid())
        for _, n in pairs(e:GetAllComponentNames(false)) do
            if tostring(n):find("IsSneaking", 1, true) then return true end
        end
        return false
    end)
    return ok and yes
end
-- The living mirrored mob's stand-in BG3's cursor picks, if any.
local function pickedStandIn()
    local ok, uuid = pcall(function()
        return tostring(Ext.UI.GetPickingHelper(1).Inner.Inner[1].Character.Uuid.EntityUuid)
    end)
    return ok and standIns[uuid] == true and uuid or nil
end
local function picksStandIn() return pickedStandIn() ~= nil end
-- BG3's own cursor choice is BG3's action on the NPC: a melee or ranged attack, a spell, a throw or a
-- shove, or a pickpocket (BG3's left click while sneaking: a playtest report, it talked instead).
M.AttackCursors = {"Melee", "Bow", "Wand", "Cast", "Throw", "Shove", "Attack", "PickPocket"}
local function attackCursor(name)
    for _, w in ipairs(M.AttackCursors) do
        if tostring(name):find(w, 1, true) then return true end
    end
    return false
end
-- Mod-wide (WorldMode.lua's click hook reads it): BG3 attacks the friendly NPC under the pointer.
WOWBG3BG3TakesClick = false

local function hover()
    local raw = Ext.IO.LoadFile("wow-bg3/link/wowhover.bin")
    if not raw or #raw < 28 then return nil end
    local tag, seq, flags, seq2, kind, unable, seq3 = string.unpack("<c4I4I4I4I4I4I4", raw)
    if tag ~= "WBH1" or seq ~= seq2 or seq ~= seq3 then return nil end
    local quest, unit = 0, nil
    if #raw >= 36 then
        local q, seq4 = string.unpack("<I4I4", raw, 29)
        if seq4 == seq then quest = q end
    end
    if #raw >= 48 then
        local lo, hi, seq5 = string.unpack("<I4I4I4", raw, 37)
        if seq5 == seq and (lo ~= 0 or hi ~= 0) then unit = string.format("%08x%08x", hi, lo) end
    end
    return flags, KINDS[kind], unable ~= 0, quest, unit
end
M.Hover = hover

-- The quest cursors are usable (see the header): the plugin is loaded and all four files are there.
local QUEST_FILES = {"quest-available.cur", "quest-available-pressed.cur", "quest-turnin.cur", "quest-turnin-pressed.cur"}
-- WoW's merchant cursor over a vendor (a playtest report: it showed BG3's Talk): benilla 0064 writes
-- merchant*.cur and reports a vendor as Buy; the wowbg3-early plugin swaps BG3's OpenDoor cursors (no
-- BG3 door in world mode) for them. Checked every 300 ticks until the files are there.
local MERCHANT_FILES = {"merchant.cur", "merchant-pressed.cur", "merchant-gray.cur", "merchant-gray-pressed.cur"}
M.MerchantInstalled = false
function M.InstallMerchantCursor()
    local ok = M.QuestInstalled
    for _, f in ipairs(MERCHANT_FILES) do
        local raw = ok and Ext.IO.LoadFile("wow-bg3/cursors/" .. f)
        ok = ok and raw ~= nil and #raw == 4286
    end
    M.MerchantInstalled = ok == true
    return M.MerchantInstalled
end

-- `again`: a later look for the files (a new install's first session makes them while the game runs; the early
-- plugin reads each file when BG3 first sets its cursor): says nothing while they are still missing.
function M.InstallQuestCursors(again)
    local st = {}
    local ok, err = pcall(function()
        for _, f in ipairs(QUEST_FILES) do
            local raw = Ext.IO.LoadFile("wow-bg3/cursors/" .. f)
            -- A 32 x 32 32-bit .cur as BG3's own (6 + 16 + 40 + 4096 + 128 bytes, type 2).
            assert(raw and #raw == 4286 and string.unpack("<I2", raw, 3) == 2, "missing or not a 32 x 32 cursor: " .. f)
        end
        local plugin
        for _, p in ipairs(Ext.Plugins.List()) do
            if p.Name == "wowbg3-early" then plugin = p end
        end
        assert(plugin and plugin.Loaded, "bg3le plugin wowbg3-early not loaded")
        st.Plugin = plugin.Version
    end)
    M.QuestInstalled = ok
    if again and not ok then return false end
    st.Installed, st.Error = ok, (not ok) and tostring(err) or nil
    M.QuestStatus = st
    Ext.IO.SaveFile("wow-bg3/cursor-status.json", Ext.Json.Stringify(st))
    Ext.Log.Print("WOW-BG3 cursor: WoW quest cursors " .. (ok and "in" or ("off: " .. tostring(err))))
    return ok
end

-- One tick's decision (exported for the harness). flags, kind, unable, quest: wowhover.bin; own:
-- BG3's RequestedCursor now; picked: the stand-in BG3 picks (or nil); friendlyPick: it is a
-- friendly NPC's; written: what this module wrote last. Returns the cursor wanted (or nil), the
-- mode ("Requested" or "Override") and whether BG3 takes a left click (a friendly NPC attacked).
function M.Decide(flags, kind, unable, quest, own, picked, friendlyPick, written)
    local want, mode = nil, M.Mode
    local bg3Attacks = friendlyPick and attackCursor(own)
    if bg3Attacks then
        mode = M.MirroredMode -- no override: BG3's own cursor shows
    elseif flags and flags & 2 ~= 0 and flags & 1 == 0 and kind and BG3[kind] then
        local mark = M.QuestInstalled and kind ~= "Attack" and QUEST[quest or 0] or nil
        if mark then
            want = M.QuestCursors[mark]
        elseif kind == "Buy" and M.MerchantInstalled then
            want = unable and "OpenDoorWarning" or "OpenDoor"
        else
            want = BG3[kind] .. ((unable and BG3[kind] ~= "Cast") and "Warning" or "")
        end
        -- BG3 rewrites RequestedCursor every frame over a stand-in it picks.
        if friendlyPick then mode = M.MirroredMode end
    elseif flags and flags & 1 == 0 then
        -- (A friendly NPC's stand-in never shows the sword: that one is BG3's attack only.)
        local mirrored = flags & 4 ~= 0 or (picked ~= nil and not friendlyPick)
        -- BG3 wrote its own choice after the last tick (see the header), unless it left ours.
        if mirrored and (own == "Arrow" or own == written) then
            want = (flags & 4 ~= 0 and unable) and "MeleeWarning" or "Melee"
            mode = M.MirroredMode
        end
    end
    return want, mode, bg3Attacks == true
end

-- A left click on a friendly NPC (see the header): tgt {Uuid, Guid, Wow (WoW picks it), Bg3 (BG3 picks
-- it)}. Returns the cursor wanted, the mode, whether BG3 takes the click itself and what the click
-- does: "bg3" (BG3's own: an attack, or its pickpocket on the stand-in it picks), "pickpocket",
-- "talk" or "none" (nothing WoW's: BG3's plain click).
-- The cursor WoW gives a friendly NPC (its hover kind and quest mark), kept per guid: over the stand-in
-- alone (BG3's pick, not WoW's) the same NPC showed a plain Talk where its WoW model showed the quest or
-- merchant cursor (a playtest report: "the quest icon turns into the talk icon"). WoW's too-far warning
-- (the red Talk) is not used for them: a click walks the hero there (Interact.lua).
local wowLook = {} -- guid -> {Flags, Kind, Quest}
function M.DecideFriendly(flags, kind, unable, quest, own, tgt, sneaks, talker)
    local mode = tgt.Bg3 and M.MirroredMode or M.Mode
    if tgt.Wow and flags and flags & 2 ~= 0 and kind then
        wowLook[tgt.Guid] = {Flags = flags, Kind = kind, Quest = quest}
        noteQuest(tgt.Guid, quest)
        if talker ~= nil then talker = talks(tgt.Guid) end
    elseif not tgt.Wow and wowLook[tgt.Guid] then
        local l = wowLook[tgt.Guid]
        flags, kind, quest = l.Flags, l.Kind, l.Quest
        tgt = {Uuid = tgt.Uuid, Guid = tgt.Guid, Bg3 = tgt.Bg3, Wow = true}
    end
    unable = false
    if attackCursor(own) and not tostring(own):find("PickPocket", 1, true) then return nil, M.MirroredMode, true, "bg3" end
    if sneaks then
        return "PickPocket", mode, tgt.Bg3, tgt.Bg3 and "bg3" or "pickpocket"
    end
    if talker == false then return nil, mode, false, "none" end
    local want = "Talk"
    if tgt.Wow and flags and flags & 2 ~= 0 and kind and BG3[kind] and kind ~= "Attack" then
        local mark = M.QuestInstalled and QUEST[quest or 0] or nil
        if mark then want = M.QuestCursors[mark]
        elseif kind == "Buy" and M.MerchantInstalled then want = unable and "OpenDoorWarning" or "OpenDoor"
        elseif BG3[kind] == "Talk" then want = "Talk" .. (unable and "Warning" or "") end
    end
    return want, mode, false, "talk"
end
-- Mod-wide (WorldMode.lua's click hook reads it): this tick's friendly NPC click, or nil.
WOWBG3FriendlyClick = nil
-- The module for the harness's research chunks (Ext.Require does not resolve from them).
WOWBG3Cursor = M

-- The stand-in under WoW's model (StandInAim.lua): named to the plugin, so BG3's own pick finds it.
local aimState, aimBroken = {}, nil
local function feetOf(uuid)
    local ok, p = pcall(function()
        local t = Ext.Entity.Get(uuid).Transform.Transform.Translate
        return {t[1], t[2], t[3]}
    end)
    return ok and p or nil
end
local function aimCall(mode, x, y, z)
    if aimBroken then return nil end
    local ok, out = pcall(N.Call, N.Op.StandInAim, mode, x, y, z)
    if ok and out.Err == 0 then return out end
    -- An older plugin without op 32, or no channel: BG3 picks as before; said once.
    aimBroken = ok and tostring(out.Why or out.Err) or tostring(out)
    Ext.Log.Print("WOW-BG3 cursor: stand-in aim unavailable: " .. aimBroken)
    return nil
end
local function aimStep(target)
    local line = Aim.Step(aimState, target, feetOf, aimCall)
    M.Aim = {Uuid = aimState.Uuid, Out = aimState.Out and aimState.Out.R or nil, Broken = aimBroken}
    if line then Ext.Log.Print("WOW-BG3 cursor: " .. line) end
end

function M.Start(runId)
    M.InstallQuestCursors()
    local ticks, overridden, written = 0, false, nil
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if not M.QuestInstalled and ticks % 300 == 0 then pcall(M.InstallQuestCursors, true) end
        if not M.MerchantInstalled and ticks % 300 == 1 then pcall(M.InstallMerchantCursor) end
        if not (World.IsActive and World.IsActive()) then
            if overridden then pcall(function() local c = Ext.UI.GetCursorControl(); c.CursorOverrides = {} end); overridden = false end
            M.FriendlyAttack = false
            pcall(aimStep, nil)
            return
        end
        local ok, err = pcall(function()
            local flags, kind, unable, quest, unit = hover()
            local c = Ext.UI.GetCursorControl()
            -- Friendly NPCs (see the header): BG3's attack cursor over its stand-in wins.
            refreshStandIns(ticks)
            -- A BG3 window under the pointer's way (checked every 6 ticks): BG3's alone.
            if ticks % 6 == 0 then M.Window = World.Bg3WindowOpen and World.Bg3WindowOpen() or false end
            -- WoW's model of a mob or NPC under the pointer: BG3's pick finds its stand-in (next frame's query).
            aimStep(Aim.Target(flags, unit, M.Window, byGuid))
            local picked = flags and flags & 1 == 0 and pickedStandIn() or nil
            local friendlyPick = picked ~= nil and friendlies[picked] ~= nil
            local own = tostring(c.RequestedCursor)
            local tgt = nil
            if not M.Window and flags and flags & 1 == 0 then
                local wowUuid = unit and flags & 2 ~= 0 and friendlyGuids[unit] or nil
                if wowUuid then tgt = {Uuid = wowUuid, Guid = unit, Wow = true, Bg3 = picked == wowUuid}
                elseif friendlyPick then tgt = {Uuid = picked, Guid = friendlies[picked], Bg3 = true} end
            end
            local want, mode, bg3Attacks, click
            if tgt then
                want, mode, bg3Attacks, click = M.DecideFriendly(flags, kind, unable, quest, own, tgt, sneaking(), talks(tgt.Guid))
            elseif M.Window then
                want, mode, bg3Attacks = nil, M.Mode, false
            else
                want, mode, bg3Attacks = M.Decide(flags, kind, unable, quest, own, picked, friendlyPick, written)
            end
            WOWBG3BG3TakesClick = bg3Attacks
            WOWBG3FriendlyClick = tgt and click and {Mode = click, Uuid = tgt.Uuid, Guid = tgt.Guid} or nil
            M.Friendly = WOWBG3FriendlyClick
            M.FriendlyAttack = bg3Attacks
            M.FriendlyPick = friendlyPick and own or nil
            M.Flags, M.Quest = flags, quest
            local id = want and cursorId(want)
            if mode == "Override" and (id or not want) then
                if want then c.CursorOverrides = {id}; overridden = true
                elseif overridden then c.CursorOverrides = {}; overridden = false end
            else
                if overridden then c.CursorOverrides = {}; overridden = false end
                if want then c.RequestedCursor = want; written = want
                elseif M.Window and written then
                    -- The cursor this module wrote last stays BG3's requested one in its windows, where
                    -- BG3 does not rewrite it (a playtest report: the Talk bubble through the whole
                    -- caught-stealing dialog, its options and dice rolls): back to BG3's plain arrow once.
                    if own == written then c.RequestedCursor = "Arrow" end
                    written = nil
                end
            end
            M.Last = want
        end)
        if not ok and ticks % 300 == 0 then Ext.Log.Print("WOW-BG3 cursor: " .. tostring(err)) end
    end)
end
return M
